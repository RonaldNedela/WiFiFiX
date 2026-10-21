/*
 * LegacyWiFiFix -- Fix WPA2/WPA3 transitional Wi-Fi association on iOS 3.x - 12.x
 *
 * Prevents wifid from crashing / returning -0xF3C (0xFFFFF0C4) when encountering
 * AKM 8 (SAE / WPA3-Personal) or other modern AKMs on mixed networks by stripping
 * unhandled AKMs from IE_KEY_RSN_AUTHSELS so wifid associates via standard WPA2-PSK.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <syslog.h>
#include <dlfcn.h>
#include <stdbool.h>

#ifndef TAG
#define TAG "LegacyWiFiFix"
#endif

#include "finder.h"

#ifndef kCFCoreFoundationVersionNumber_iOS_8_0
#define kCFCoreFoundationVersionNumber_iOS_8_0 1140.10
#endif
#ifndef kCFCoreFoundationVersionNumber_iOS_6_0
#define kCFCoreFoundationVersionNumber_iOS_6_0 793.00
#endif

static int g_max_known_akm = 6;

static void *findRSN_IE_Parser(void) {
    uint32_t count = _dyld_image_count();
    syslog(LOG_NOTICE, TAG ": scanning %u loaded images", count);
    for (uint32_t i = 0; i < count; i++) {
        const mach_header_t *header = (const mach_header_t *)_dyld_get_image_header(i);
        if (!header) continue;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const char *name = _dyld_get_image_name(i);

        uintptr_t func = findRSN_IE_ParserInImage(header, slide);
        if (func) {
            syslog(LOG_NOTICE, TAG ": located parser in image %u (%s)",
                   i, name ? name : "(null)");
            return FUNC_PTR(func);
        }
    }
    return NULL;
}

#define LOG_BUDGET 16
static unsigned g_log_used = 0;

static inline bool logBudgetOk(void) {
    if (g_log_used < LOG_BUDGET) { g_log_used++; return true; }
    if (g_log_used == LOG_BUDGET) {
        syslog(LOG_NOTICE, TAG ": further per-BSS filter logs suppressed");
        g_log_used++;
    }
    return false;
}

static void filterAuthSels(CFMutableDictionaryRef rsnIE) {
    CFArrayRef authSels = (CFArrayRef)CFDictionaryGetValue(rsnIE, CFSTR("IE_KEY_RSN_AUTHSELS"));
    if (!authSels) return;

    CFIndex count = CFArrayGetCount(authSels);
    if (count == 0) return;

    // Fast path: avoid heap allocation if all AKMs are already valid.
    // Over 99% of Wi-Fi beacons require no filtering.
    bool needsFiltering = false;
    for (CFIndex i = 0; i < count; i++) {
        CFTypeRef elem = CFArrayGetValueAtIndex(authSels, i);
        if (!elem || CFGetTypeID(elem) != CFNumberGetTypeID()) {
            needsFiltering = true;
            break;
        }
        int32_t akm = 0;
        if (!CFNumberGetValue((CFNumberRef)elem, kCFNumberSInt32Type, &akm) ||
            akm < 1 || akm > g_max_known_akm) {
            needsFiltering = true;
            break;
        }
    }

    if (!needsFiltering) {
        return;
    }

    CFMutableArrayRef filtered = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!filtered) return;

    bool didFilter = false;
    bool logOk = logBudgetOk();
    for (CFIndex i = 0; i < count; i++) {
        CFTypeRef elem = CFArrayGetValueAtIndex(authSels, i);
        if (!elem || CFGetTypeID(elem) != CFNumberGetTypeID()) {
            didFilter = true;
            if (logOk)
                syslog(LOG_NOTICE, TAG ": stripped non-CFNumber element from RSN IE");
            continue;
        }
        CFNumberRef num = (CFNumberRef)elem;
        int32_t akm = 0;
        if (CFNumberGetValue(num, kCFNumberSInt32Type, &akm)
            && akm >= 1 && akm <= g_max_known_akm) {
            CFArrayAppendValue(filtered, num);
        } else {
            didFilter = true;
            if (logOk)
                syslog(LOG_NOTICE, TAG ": stripped unknown AKM %d from RSN IE", (int)akm);
        }
    }

    if (didFilter && CFArrayGetCount(filtered) > 0) {
        CFDictionarySetValue(rsnIE, CFSTR("IE_KEY_RSN_AUTHSELS"), filtered);
        if (logOk)
            syslog(LOG_NOTICE, TAG ": kept %ld of %ld AKM selectors",
                   (long)CFArrayGetCount(filtered), (long)count);
    } else if (didFilter && logOk) {
        syslog(LOG_WARNING, TAG ": all AKMs were unknown -- leaving unmodified");
    }

    CFRelease(filtered);
}

typedef int (*rsn_ie_parser_t)(const char *, int, CFMutableDictionaryRef);
static rsn_ie_parser_t orig_parseRSN_IE = NULL;

static int hooked_parseRSN_IE(const char *rawIE, int totalLen, CFMutableDictionaryRef output) {
    int ret = orig_parseRSN_IE(rawIE, totalLen, output);
    if (ret != 0 || !output) return ret;

    CFTypeRef rsnIE = CFDictionaryGetValue(output, CFSTR("RSN_IE"));
    if (rsnIE && CFGetTypeID(rsnIE) == CFDictionaryGetTypeID()) {
        filterAuthSels((CFMutableDictionaryRef)rsnIE);
    }
    return ret;
}

typedef void (*MSHookFunction_t)(void *symbol, void *replace, void **result);

static MSHookFunction_t resolveHookFunction(void) {
    MSHookFunction_t fn = (MSHookFunction_t)dlsym(RTLD_DEFAULT, "MSHookFunction");
    if (fn) return fn;

    const char *candidates[] = {
        "/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate",
        "/usr/lib/libsubstrate.dylib",
        "/usr/lib/libsubstitute.dylib",
        "/usr/lib/libhooker.dylib",
        "/usr/lib/libellekit.dylib",
        "/usr/lib/ellekit/libellekit.dylib",
        "/usr/lib/libhooker/libhooker.dylib",
        "/var/jb/usr/lib/libellekit.dylib",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        void *h = dlopen(candidates[i], RTLD_LAZY | RTLD_GLOBAL);
        if (h) {
            fn = (MSHookFunction_t)dlsym(h, "MSHookFunction");
            if (fn) return fn;
        }
    }
    return NULL;
}

__attribute__((constructor))
static void init_legacy_wifi_fix(void) {
    if (kCFCoreFoundationVersionNumber >= kCFCoreFoundationVersionNumber_iOS_8_0) {
        g_max_known_akm = 6;
    } else if (kCFCoreFoundationVersionNumber >= kCFCoreFoundationVersionNumber_iOS_6_0) {
        g_max_known_akm = 4;
    } else {
        g_max_known_akm = 2;
    }
    syslog(LOG_NOTICE, TAG ": CFVersion=%.1f -> max known AKM = %d",
           kCFCoreFoundationVersionNumber, g_max_known_akm);

    void *target = findRSN_IE_Parser();
    if (!target) {
        syslog(LOG_ERR, TAG ": could not locate RSN IE parser -- hook not installed");
        return;
    }

    MSHookFunction_t hookFn = resolveHookFunction();
    if (!hookFn) {
        syslog(LOG_ERR, TAG ": could not resolve MSHookFunction from Substrate / Substitute / libhooker");
        return;
    }

    hookFn(target, (void *)hooked_parseRSN_IE, (void **)&orig_parseRSN_IE);
    syslog(LOG_NOTICE, TAG ": hook installed successfully at %p", target);
}
