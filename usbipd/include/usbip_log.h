#ifndef USBIP_LOG_H
#define USBIP_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* USBIP_LOG_LEVEL: 0=errors/minimal, 1=normal (default), 2=verbose, 3=transfer trace */
static int usbip_log_level(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    const char *value = getenv("USBIP_LOG_LEVEL");
    if (!value || !*value) cached = 1;
    else if (!strcmp(value, "0") || !strcmp(value, "error")) cached = 0;
    else if (!strcmp(value, "1") || !strcmp(value, "info")) cached = 1;
    else if (!strcmp(value, "2") || !strcmp(value, "debug")) cached = 2;
    else if (!strcmp(value, "3") || !strcmp(value, "trace")) cached = 3;
    else cached = 1;
    return cached;
}

#define USBIP_LOG(level, ...) do { if (usbip_log_level() >= (level)) { printf(__VA_ARGS__); } } while (0)

#endif
