#include <libusb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

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

typedef enum {NONE = 0, WIRED, DONGLE} DeviceType;
typedef struct
{ 
   DeviceType type;
   unsigned char command_prefix;
   libusb_device_handle *handle;
   short initialized;
} Device;

static void signal_handler()
{
    RUNNING = 0;
}

static int send(Device *device, unsigned int length, ...)
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
    send(device, 5, 0x08, 0x01, 0x03, 0x00, 0x02);
    if (device->type == DONGLE) 
        send(device, 5, 0x09, 0x01, 0x03, 0x00, 0x02);
    send(device, 4, device->command_prefix, 0x0d, 0x00, 0x01);

    /* Set custom configuration */
    send(device, 12, device->command_prefix,
         0x06, 0x00, 0x06, 0x00, 0x00, 0x00,
         0x00,  /* Indicator LED's red */
         0x00,  /* Main LED's red */
         0xff,  /* Indicator LED's green */
         0x00,  /* Main LED's green */
         0x00,  /* Indicator LED's blue */
         0x00); /* Main LED's blue */

    send(device, 5, 0x01, 0x20, 0x00, 0x08, 0x70); /* Set DPI (708 hex = 1800) */

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
    send(device, 2, device->command_prefix, 0x12);
    ungrab_device(device);
}

int main()
{
    int r;
    Device device;
    libusb_hotplug_callback_handle hp[4];

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
