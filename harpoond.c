#include <libusb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

uint16_t VENDOR_ID = 0x1b1c;
uint16_t WIRED_DEVICE_ID = 0x1b5e;
uint16_t DONGLE_DEVICE_ID = 0x1b65;
unsigned char COMMAND_PREFIX = 0x08;
short WIRED = 1;
static volatile short RUNNING = 1;

#define BUFFER_SIZE     64
#define ENDPOINT_OUT    0x04
#define ENDPOINT_IN     0x84

void signal_handler()
{
    RUNNING = 0;
}

int send(libusb_device_handle *dev, unsigned int length, ...)
{
    int r;
    int transferred;
    unsigned char buffer[BUFFER_SIZE];
    va_list valist;

    va_start(valist, length);
    for (unsigned int i = 0; i < BUFFER_SIZE; i++)
        buffer[i] = i < length ? va_arg(valist, int) : 0x00;
    va_end(valist);

    r = libusb_interrupt_transfer(dev, ENDPOINT_OUT, buffer, BUFFER_SIZE, &transferred, 100);
    if (transferred < BUFFER_SIZE) {
        fprintf(stderr, "short write (%d, %d)\n", transferred, r);
        return -1;
    }

    r = libusb_interrupt_transfer(dev, ENDPOINT_IN, buffer, BUFFER_SIZE, &transferred, 100);
    if (transferred < BUFFER_SIZE) {
        fprintf(stderr, "short read (%d, %d)\n", transferred, r);
        return -1;
    }

    return 0;
}

int grab_device(libusb_device_handle *dev)
{
    if (libusb_kernel_driver_active(dev, 1) == 1) {
        int r = libusb_detach_kernel_driver(dev, 1);
        if (r < 0) return r;
    }

    return libusb_claim_interface(dev, 1);
}

int ungrab_device(libusb_device_handle *dev)
{
    int r;

    r = libusb_release_interface(dev, 1);
    if (r < 0) return r;
    r = libusb_attach_kernel_driver(dev, 1);
    if (r < 0) return r;

    return 0;
}

int main()
{
    int r;
    libusb_device_handle *dev;

    r = libusb_init(NULL);
    if (r < 0) return r;

    dev = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, WIRED_DEVICE_ID);

    if (dev == NULL) {
        dev = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, DONGLE_DEVICE_ID);
        WIRED = 0;
        COMMAND_PREFIX = 0x09;
    }
    if (dev == NULL) {
        fprintf(stderr, "Device not found\n");
        return -1;
    }

    grab_device(dev);

    /* Init */
    send(dev, 5, 0x08, 0x01, 0x03, 0x00, 0x02);
    if (WIRED) {
        send(dev, 4, 0x08, 0x0d, 0x00, 0x01);
    } else {
        send(dev, 5, 0x09, 0x01, 0x03, 0x00, 0x02);
        send(dev, 4, 0x09, 0x0d, 0x00, 0x01);
    }

    /* Set my configuration */
    send(dev, 12, COMMAND_PREFIX, 0x06, 0x00, 0x06, 0x00, 0x00, 0x00,
         0x00,  /* Indicator LED's red */
         0x00,  /* Main LED's red */
         0xff,  /* Indicator LED's green */
         0x00,  /* Main LED's green */
         0x00,  /* Indicator LED's blue */
         0x00); /* Main LED's blue */

    send(dev, 5, 0x01, 0x20, 0x00, 0x08, 0x70); /* Set DPI (708 hex = 1800) */

    ungrab_device(dev);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Keep alive */
    while (RUNNING) {
        grab_device(dev);
        send(dev, 2, COMMAND_PREFIX, 0x12);
        ungrab_device(dev);
        sleep(55);
    }

    printf("Cleaning up...\n");
    libusb_close(dev);
    libusb_exit(NULL);
    return 0;
}
