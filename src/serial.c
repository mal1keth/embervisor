/* SPDX-License-Identifier: MIT
 *
 * 8250/16550A UART emulation at COM1 (ports 0x3f8-0x3ff, ISA IRQ 4).
 *
 * This is the guest's console. TX bytes go to our stdout; a reader
 * thread feeds host stdin into the RX FIFO and pulses IRQ 4 through
 * KVM's in-kernel PIC/IOAPIC (KVM_IRQ_LINE), which is what makes the
 * guest shell *interactive*: the guest's 8250 driver sleeps until the
 * RX interrupt wakes it.
 *
 * Register model implemented: RBR/THR, IER, IIR/FCR, LCR (incl. DLAB
 * divisor latch), MCR (incl. loopback, which the kernel's autoconfig
 * probe uses), LSR, MSR, SCR. Transmit is instantaneous, so LSR always
 * reports THRE|TEMT, and the "16550A" the guest detects never drops a
 * byte.
 */
#include "embervisor.h"

#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#define COM1_BASE 0x3f8
#define RX_RING   4096

/* Register offsets from COM1_BASE. */
enum {
    UART_RBR_THR_DLL = 0,   /* read: RBR, write: THR; DLL when DLAB=1 */
    UART_IER_DLM     = 1,   /* interrupt enable;      DLM when DLAB=1 */
    UART_IIR_FCR     = 2,   /* read: int id, write: FIFO control      */
    UART_LCR         = 3,   /* line control (bit 7 = DLAB)            */
    UART_MCR         = 4,   /* modem control (bit 4 = loopback)       */
    UART_LSR         = 5,   /* line status                            */
    UART_MSR         = 6,   /* modem status                           */
    UART_SCR         = 7,   /* scratch                                */
};

#define IER_RDI   0x01      /* rx data available interrupt   */
#define IER_THRI  0x02      /* tx holding empty interrupt    */
#define LCR_DLAB  0x80
#define MCR_LOOP  0x10
#define LSR_DR    0x01
#define LSR_THRE  0x20
#define LSR_TEMT  0x40

struct serial {
    pthread_mutex_t lock;
    struct vm *vm;

    uint8_t dll, dlm, ier, fcr, lcr, mcr, scr;
    bool thri_pending;      /* "tx empty" edge armed and not yet reported */
    int irq_level;          /* last level fed to KVM_IRQ_LINE             */

    uint8_t rx[RX_RING];    /* ring buffer, reader thread to guest        */
    unsigned rx_head, rx_tail;

    pthread_t reader;
    int stop_pipe[2];
    bool stdin_is_tty;
};

static struct termios saved_termios;
static bool termios_saved;

static void restore_terminal(void)
{
    if (termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

static unsigned rx_count(struct serial *s)
{
    return s->rx_head - s->rx_tail;
}

static void rx_push(struct serial *s, uint8_t c)
{
    if (rx_count(s) < RX_RING)
        s->rx[s->rx_head++ % RX_RING] = c;
    /* else: RX overrun; a real 8250 would set LSR.OE; we just drop.  */
}

static uint8_t rx_pop(struct serial *s)
{
    if (rx_count(s) == 0)
        return 0;
    return s->rx[s->rx_tail++ % RX_RING];
}

/*
 * Recompute the interrupt line from current state, and tell the
 * in-kernel irqchip if it changed. IIR priority: RX data beats TX-empty.
 */
static void update_irq(struct serial *s)
{
    int level = 0;

    if ((s->ier & IER_RDI) && rx_count(s))
        level = 1;
    else if ((s->ier & IER_THRI) && s->thri_pending)
        level = 1;

    if (!s->vm->has_irqchip)
        return;
    if (level == s->irq_level && !level)
        return;
    s->irq_level = level;

    struct kvm_irq_level irq = { .irq = EMBER_SERIAL_IRQ, .level = level };
    if (ioctl(s->vm->fd, KVM_IRQ_LINE, &irq) < 0)
        die("KVM_IRQ_LINE: %m");
}

bool serial_owns_port(uint16_t port)
{
    return port >= COM1_BASE && port < COM1_BASE + 8;
}

static uint8_t reg_read(struct serial *s, unsigned off)
{
    uint8_t v = 0;

    switch (off) {
    case UART_RBR_THR_DLL:
        if (s->lcr & LCR_DLAB)
            return s->dll;
        v = rx_pop(s);
        update_irq(s);
        return v;
    case UART_IER_DLM:
        return (s->lcr & LCR_DLAB) ? s->dlm : s->ier;
    case UART_IIR_FCR:
        if ((s->ier & IER_RDI) && rx_count(s))
            v = 0x04;                           /* RX data available */
        else if ((s->ier & IER_THRI) && s->thri_pending)
            v = 0x02, s->thri_pending = false;  /* reading IIR clears it */
        else
            v = 0x01;                           /* no interrupt pending  */
        if (s->fcr & 0x01)
            v |= 0xc0;                          /* FIFOs enabled: 16550A */
        update_irq(s);
        return v;
    case UART_LCR:
        return s->lcr;
    case UART_MCR:
        return s->mcr;
    case UART_LSR:
        return LSR_THRE | LSR_TEMT | (rx_count(s) ? LSR_DR : 0);
    case UART_MSR:
        if (s->mcr & MCR_LOOP)                  /* loopback: MSR mirrors MCR */
            return ((s->mcr & 0x01) ? 0x20 : 0) |   /* DTR -> DSR */
                   ((s->mcr & 0x02) ? 0x10 : 0) |   /* RTS -> CTS */
                   ((s->mcr & 0x04) ? 0x40 : 0) |   /* OUT1 -> RI */
                   ((s->mcr & 0x08) ? 0x80 : 0);    /* OUT2 -> DCD */
        return 0xb0;                            /* DCD|DSR|CTS: happy cable */
    case UART_SCR:
        return s->scr;
    }
    return 0;
}

static void reg_write(struct serial *s, unsigned off, uint8_t v)
{
    switch (off) {
    case UART_RBR_THR_DLL:
        if (s->lcr & LCR_DLAB) {
            s->dll = v;
            break;
        }
        if (s->mcr & MCR_LOOP) {
            rx_push(s, v);                      /* TX loops back to RX */
        } else {
            ssize_t n = write(STDOUT_FILENO, &v, 1);
            (void)n;
        }
        if (s->ier & IER_THRI)
            s->thri_pending = true;             /* byte sent, THR empty again */
        update_irq(s);
        break;
    case UART_IER_DLM:
        if (s->lcr & LCR_DLAB) {
            s->dlm = v;
            break;
        }
        s->ier = v & 0x0f;
        if ((s->ier & IER_THRI))
            s->thri_pending = true;             /* THRE already set => fires */
        update_irq(s);
        break;
    case UART_IIR_FCR:
        s->fcr = v;
        if (v & 0x02) {                         /* clear RX FIFO */
            s->rx_tail = s->rx_head;
            update_irq(s);
        }
        break;
    case UART_LCR: s->lcr = v; break;
    case UART_MCR: s->mcr = v; break;
    case UART_SCR: s->scr = v; break;
    /* LSR/MSR writes: read-only in this model, ignore. */
    }
}

void serial_io(struct serial *s, uint16_t port, bool is_write,
               uint8_t *data, uint32_t size)
{
    unsigned off = port - COM1_BASE;

    pthread_mutex_lock(&s->lock);
    /*
     * The 8250 has byte-wide registers; the kernel only ever does
     * 1-byte accesses here. Wider accesses would be a guest bug, so we
     * service byte 0 and float the rest.
     */
    if (is_write)
        reg_write(s, off, data[0]);
    else {
        data[0] = reg_read(s, off);
        if (size > 1)
            memset(data + 1, 0xff, size - 1);
    }
    pthread_mutex_unlock(&s->lock);
}

/*
 * Reader thread: host stdin to RX FIFO to IRQ 4. Runs raw so every
 * keystroke (including ^C, which the *guest* gets, try it) goes to the
 * guest. Escape hatch: Ctrl-A then `x` quits, Ctrl-A Ctrl-A sends a
 * literal Ctrl-A, same convention as QEMU's -nographic.
 */
static void *reader_main(void *arg)
{
    struct serial *s = arg;
    bool saw_ctrl_a = false;

    for (;;) {
        fd_set rfds;
        int maxfd = s->stop_pipe[0];

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(s->stop_pipe[0], &rfds);
        if (STDIN_FILENO > maxfd)
            maxfd = STDIN_FILENO;

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue;
            return NULL;
        }
        if (FD_ISSET(s->stop_pipe[0], &rfds))
            return NULL;

        uint8_t buf[512];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return NULL;        /* EOF: stop feeding, guest keeps running */

        pthread_mutex_lock(&s->lock);
        for (ssize_t i = 0; i < n; i++) {
            uint8_t c = buf[i];

            if (s->stdin_is_tty) {
                if (saw_ctrl_a) {
                    saw_ctrl_a = false;
                    if (c == 'x' || c == 'X') {
                        pthread_mutex_unlock(&s->lock);
                        fprintf(stderr, "\nembervisor: quit (Ctrl-A x)\n");
                        exit(0);        /* atexit() restores the terminal */
                    }
                    /* Ctrl-A <anything else>: fall through, deliver c.  */
                } else if (c == 0x01) {
                    saw_ctrl_a = true;
                    continue;
                }
            }
            rx_push(s, c);
        }
        update_irq(s);
        pthread_mutex_unlock(&s->lock);
    }
}

struct serial *serial_create(struct vm *vm)
{
    struct serial *s = calloc(1, sizeof(*s));

    if (!s)
        die("calloc serial");
    s->vm = vm;
    pthread_mutex_init(&s->lock, NULL);
    if (pipe(s->stop_pipe) < 0)
        die("pipe: %m");

    s->stdin_is_tty = isatty(STDIN_FILENO);
    if (s->stdin_is_tty) {
        struct termios raw;

        if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
            termios_saved = true;
            atexit(restore_terminal);
            raw = saved_termios;
            cfmakeraw(&raw);
            raw.c_oflag |= OPOST | ONLCR;   /* keep host-side \n sane */
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
    }

    if (pthread_create(&s->reader, NULL, reader_main, s) != 0)
        die("pthread_create: %m");

    info("serial: 8250 at 0x3f8 irq %d (stdin %s)", EMBER_SERIAL_IRQ,
         s->stdin_is_tty ? "tty, raw mode" : "not a tty");
    return s;
}
