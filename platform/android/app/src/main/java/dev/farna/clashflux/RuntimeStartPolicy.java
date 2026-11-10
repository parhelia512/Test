package dev.farna.clashflux;

/** Request authority is separate from persisted crash-recovery intent. */
final class RuntimeStartPolicy {
    static boolean preserveLiveVpn(boolean started, boolean starting, boolean requested,
                                   boolean stopping, boolean coreOnly) {
        return (started || starting) && requested && !stopping && !coreOnly;
    }

    static boolean mayStart(boolean stopping, boolean started, boolean starting,
                            boolean hasServer, boolean hasTunnel) {
        return !stopping || !(started || starting || hasServer || hasTunnel);
    }

    static boolean allowTun(boolean coreOnly, boolean requested, boolean stopping) {
        return !coreOnly && requested && !stopping;
    }

    static boolean retainInbound(boolean noTun, String type) {
        return !noTun || !"tun".equals(type);
    }
}
