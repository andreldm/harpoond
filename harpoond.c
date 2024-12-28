#include <libusb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#define VENDOR_ID             0x1b1c
#define WIRED_DEVICE_ID       0x1b5e
#define DONGLE_DEVICE_ID      0x1b65
#define BUFFER_SIZE           64
#define ENDPOINT_OUT          0x04
#define ENDPOINT_IN           0x84
#define WIRED_COMMAND_PREFIX  0x08
#define DONGLE_COMMAND_PREFIX 0x09

static volatile short RUNNING = 1;
static struct timeval zero_tv = {0};
static unsigned int r1 = 0, g1 = 0, b1 = 0; /* Main LED, Default: off */
static unsigned int r2 = 0, g2 = 255, b2 = 0; /* Indicator LED, Default: green */

/*
 * DPI values are expected in a weird format, I guess it has something to do with
 * little-endianness. For example, if 3200 DPI is desired, first it has to be
 * converted to hexadecimal, which is 0xC80, then split it in half: 0xC8, 0x00.
 *
 * More examples:
 * 1800 DPI  -> 0x708  -> 0x70, 0x08
 * 3000 DPI  -> 0xBB8  -> 0xBB, 0x08
 * 10000 DPI -> 0x2710 -> 0x27, 0x10
 *
 * Those values needs to be reversed though, this is done at init_device.
 *
 * Once you set a new DPI value, please stop harpoond it, turn  the mouse off
 * and on then start harpoond again.
 *
 * Also, note that the mouse is kind of picky with these values, some values
 * are completely ignored. I couldn't figure out any pattern here, so you
 * need to spend some time on trial & error until you get a value that works
 * and is close to what you want.
 */
static unsigned int dpi = 1800;
static unsigned int dpi_high, dpi_low;

typedef enum {NONE = 0, WIRED, DONGLE} DeviceType;
typedef struct
{ 
   DeviceType type;
   unsigned char command_prefix;
   libusb_device_handle *handle;
   short initialized;
} Device;

static void parse_arguments(int argc, char *argv[])
{
    struct option options[] = {
        {"r1", required_argument, 0, 0},
        {"g1", required_argument, 0, 0},
        {"b1", required_argument, 0, 0},
        {"r2", required_argument, 0, 0},
        {"g2", required_argument, 0, 0},
        {"b2", required_argument, 0, 0},
        {"dpi", required_argument, 0, 0},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "", options, &option_index)) != -1) {
        if (opt == 0) {
            const char *opt_name = options[option_index].name;
            int value = atoi(optarg);
            if (strcmp(opt_name, "r1") == 0) r1 = value;
            else if (strcmp(opt_name, "g1") == 0) g1 = value;
            else if (strcmp(opt_name, "b1") == 0) b1 = value;
            else if (strcmp(opt_name, "r2") == 0) r2 = value;
            else if (strcmp(opt_name, "g2") == 0) g2 = value;
            else if (strcmp(opt_name, "b2") == 0) b2 = value;
            else if (strcmp(opt_name, "dpi") == 0) {
                if (value < 0 || value > 65535) {
                    fprintf(stderr, "DPI value must be between 0 and 65535.\n");
                    exit(EXIT_FAILURE);
                }
                dpi = value;
            }
        } else {
            fprintf(stderr, "Unknown option\n");
            exit(EXIT_FAILURE);
        }
    }

    printf("Main LED: (RGB %d, %d, %d) \033[38;2;%d;%d;%dm■\033[0m\n", r1, g1, b1, r1, g1, b1);
    printf("Indicator LED: (RGB %d, %d, %d) \033[38;2;%d;%d;%dm■\033[0m\n", r2, g2, b2, r2, g2, b2);

    if (dpi <= 0xFF) {
        dpi_high = dpi;
        dpi_low = 0;
    }
    else if (dpi <= 0xFFF) {
        dpi_high = (dpi & 0xFFF) >> 4;
        dpi_low = dpi & 0xF;
    }
    else {
        dpi_high = (dpi & 0xFF00) >> 8;
        dpi_low = dpi & 0xFF;
    }
    printf("DPI: %d (Hex: 0x%X, High Byte: 0x%X, Low Byte: 0x%X)\n", dpi, dpi, dpi_high, dpi_low);
}

static void signal_handler()
{
    RUNNING = 0;
}

static int transfer(Device *device, unsigned int length, ...)
{
    int r;
    int transferred;
    unsigned char buffer[BUFFER_SIZE];
    va_list valist;

    va_start(valist, length);
    for (unsigned int i = 0; i < BUFFER_SIZE; i++)
        buffer[i] = i < length ? va_arg(valist, int) : 0x00;
    va_end(valist);

    r = libusb_interrupt_transfer(device->handle, ENDPOINT_OUT, buffer, BUFFER_SIZE, &transferred, 100);
    if (transferred < BUFFER_SIZE) {
        fprintf(stderr, "short write (%d, error %d)\n", transferred, r);
        return -1;
    }

    r = libusb_interrupt_transfer(device->handle, ENDPOINT_IN, buffer, BUFFER_SIZE, &transferred, 100);
    if (transferred < BUFFER_SIZE) {
        fprintf(stderr, "short read (%d, error %d)\n", transferred, r);
        return -1;
    }

    return 0;
}

static int grab_device(Device *device)
{
    int r;

    if (libusb_kernel_driver_active(device->handle, 1) == 1) {
        r = libusb_detach_kernel_driver(device->handle, 1);
        if (r < 0) {
            fprintf(stderr, "Failed to detach kernel driver\n");
            return r;
        }
    }

    r = libusb_claim_interface(device->handle, 1);
    if (r < 0) {
        fprintf(stderr, "Failed to claim interface\n");
        return r;
    }

    return 0;
}

static int ungrab_device(Device *device)
{
    int r;

    r = libusb_release_interface(device->handle, 1);
    if (r < 0) {
        fprintf(stderr, "Failed to release interface\n");
        return r;
    }

    r = libusb_attach_kernel_driver(device->handle, 1);
    if (r < 0) {
        fprintf(stderr, "Failed to attach kernel driver\n");
        return r;
    }

    return 0;
}

static void init_device(Device *device)
{
    if (device->handle == NULL || device->type == NONE) {
        fprintf(stderr, "Cannot initialize invalid device\n");
        return;
    }

    grab_device(device);

    /* Init */
    transfer(device, 5, 0x08, 0x01, 0x03, 0x00, 0x02);
    if (device->type == DONGLE) 
        transfer(device, 5, 0x09, 0x01, 0x03, 0x00, 0x02);
    transfer(device, 4, device->command_prefix, 0x0d, 0x00, 0x01);

    /* Set custom configuration */
    transfer(device, 13, device->command_prefix,
        0x06, 0x00, 0x06, 0x00, 0x00, 0x00, /* Do not change */
        r2,  /* Indicator LED's red */
        r1,  /* Main LED's red */
        g2,  /* Indicator LED's green */
        g1,  /* Main LED's green */
        b2,  /* Indicator LED's blue */
        b1); /* Main LED's blue */

    transfer(device, 6, device->command_prefix,
        0x01, 0x20, 0x00,   /* Do not change */
        dpi_low, dpi_high); /* DPI, inverted on purpose, explanation at the top of file */

    ungrab_device(device);

    device->initialized = 1;
}

static int LIBUSB_CALL attach_cb(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data)
{
    int r;
    Device *device;
    struct libusb_device_descriptor desc;

    (void)ctx;
    (void)dev;
    (void)event;

    printf("Device attached\n");

    device = (Device*)user_data;

    r = libusb_get_device_descriptor(dev, &desc);
    if (r < 0) {
        fprintf(stderr, "Error getting device descriptor\n");
        return 0;
    }

    r = libusb_open(dev, &device->handle);
    if (r < 0) {
        fprintf(stderr, "Error opening device\n");
        return 0;
    }

    device->type = desc.idProduct == WIRED_DEVICE_ID ? WIRED : DONGLE;
    device->command_prefix = desc.idProduct == WIRED_DEVICE_ID ? WIRED_COMMAND_PREFIX : DONGLE_COMMAND_PREFIX;
    device->initialized = 0;

    return 0;
}

static int LIBUSB_CALL detach_cb(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data)
{
    Device *device;

    (void)ctx;
    (void)dev;
    (void)event;

    printf("Device detached\n");

    device = (Device*)user_data;
    device->type = NONE;
    libusb_close(device->handle);
    device->handle = NULL;

    return 0;
}

static void keep_alive(Device *device)
{
    if (device->type == NONE) return;
    if (!device->initialized) init_device(device);

    if (grab_device(device) < 0) return;
    transfer(device, 2, device->command_prefix, 0x12);
    ungrab_device(device);
}

int main(int argc, char *argv[])
{
    int r;
    Device device;
    libusb_hotplug_callback_handle hp[4];

    parse_arguments(argc, argv);

    /*
     * INIT LIBUSB
     */
    r = libusb_init(NULL);
    if (r < 0) return r;

    /*
     * DEVICE LOOKUP
     */
    device.initialized = 0;
    device.command_prefix = WIRED_COMMAND_PREFIX;
    device.type = WIRED;
    device.handle = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, WIRED_DEVICE_ID);

    if (device.handle == NULL) {
        device.command_prefix = DONGLE_COMMAND_PREFIX;
        device.type = DONGLE;
        device.handle = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, DONGLE_DEVICE_ID);
    }

    if (device.handle == NULL) {
        device.type = NONE;
        printf("Device not found, waiting for it to be plugged\n");
    }

    /*
     * REGISTER HOTPLUG CALLBACKS
     */
    if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
            libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
                                             0, VENDOR_ID, WIRED_DEVICE_ID,
                                             LIBUSB_HOTPLUG_MATCH_ANY,
                                             attach_cb, &device, &hp[0]);
            libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
                                             0, VENDOR_ID, DONGLE_DEVICE_ID,
                                             LIBUSB_HOTPLUG_MATCH_ANY,
                                             attach_cb, &device, &hp[1]);
            libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                             0, VENDOR_ID, WIRED_DEVICE_ID,
                                             LIBUSB_HOTPLUG_MATCH_ANY,
                                             detach_cb, &device, &hp[2]);
            libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                             0, VENDOR_ID, DONGLE_DEVICE_ID,
                                             LIBUSB_HOTPLUG_MATCH_ANY,
                                             detach_cb, &device, &hp[3]);
    } else {
        printf("Hotplug capabilites are not supported on this platform\n");
    }

    /*
     * REGISTER SIGNAL HANDLERS
     */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /*
     * KEEP ALIVE AND EVENT LOOP
     */
    while (RUNNING) {
        keep_alive(&device);
        r = libusb_handle_events_timeout_completed(NULL, &zero_tv, NULL);
        if (r < 0)
            fprintf(stderr, "libusb failed to handle events: %s\n", libusb_error_name(r));
        sleep(2);
    }

    /*
     * TEAR DOWN
     */
    printf("Cleaning up...\n");
    libusb_hotplug_deregister_callback(NULL, hp[0]);
    libusb_hotplug_deregister_callback(NULL, hp[1]);
    libusb_hotplug_deregister_callback(NULL, hp[2]);
    libusb_hotplug_deregister_callback(NULL, hp[3]);
    libusb_close(device.handle);
    libusb_exit(NULL);

    return 0;
}
