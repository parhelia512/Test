package dev.farna.clashflux;

import java.util.Locale;

/** Normalizes mode intent for mobile runtime control. */
final class RuntimeModeIntent {
    static final String DEFAULT_MODE = "rule";

    private RuntimeModeIntent() {}

    static String normalize(String mode) {
        if (mode == null) return "";
        String normalized = mode.trim().toLowerCase(Locale.ROOT);
        if ("rule".equals(normalized) || "global".equals(normalized)
                || "direct".equals(normalized)) return normalized;
        return "";
    }

    static String resolve(String persisted) {
        String mode = normalize(persisted);
        return mode.isEmpty() ? DEFAULT_MODE : mode;
    }
}
