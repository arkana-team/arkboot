#include "arkboot.h"

EFI_EVENT efi_timer;
static volatile u32 milliseconds;

static void EFIAPI timer_tick(EFI_EVENT event, void *context) {
    (void)event; (void)context;
    milliseconds += 10;
}
void console_init(void) {
    EFI_SIMPLE_TEXT_OUT_PROTOCOL *out = efi_system->ConOut;
    /* Prefer a text mode large enough for the shared 16-entry menu. */
    for (INT32 mode = 0; mode < out->Mode->MaxMode; mode++) {
        UINTN columns, rows;
        if (!EFI_ERROR(out->QueryMode(out, mode, &columns, &rows)) && columns >= 80 && rows >= 25) {
            out->SetMode(out, mode);
            break;
        }
    }
    out->EnableCursor(out, FALSE);
    efi_check(efi_boot->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                   timer_tick, NULL, &efi_timer), "cannot create UEFI menu timer");
    efi_check(efi_boot->SetTimer(efi_timer, TimerPeriodic, 100000), "cannot start UEFI menu timer");
}
void print(const char *s) {
    CHAR16 buffer[128];
    while (*s) {
        u32 n = 0;
        while (*s && n < 125) {
            if (*s == '\n') buffer[n++] = '\r';
            buffer[n++] = (u8)*s++;
        }
        buffer[n] = 0;
        efi_system->ConOut->OutputString(efi_system->ConOut, buffer);
    }
}
void console_clear(void) {
    efi_system->ConOut->ClearScreen(efi_system->ConOut);
}
int console_key(void) {
    EFI_INPUT_KEY key;
    EFI_STATUS status = efi_system->ConIn->ReadKeyStroke(efi_system->ConIn, &key);
    if (status == EFI_NOT_READY) {
        efi_boot->Stall(1000);
        return -1;
    }
    efi_check(status, "UEFI keyboard read failed");
    switch (key.ScanCode) {
    case SCAN_UP: return 0x4800;
    case SCAN_DOWN: return 0x5000;
    case SCAN_HOME: return 0x4700;
    case SCAN_END: return 0x4f00;
    default: return key.UnicodeChar ? key.UnicodeChar : 0;
    }
}
u32 console_millis(void) { return milliseconds; }
