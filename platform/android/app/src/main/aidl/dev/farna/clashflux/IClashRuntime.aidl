package dev.farna.clashflux;

import android.os.Bundle;

/** Control and compact state boundary between the UI and sing-box processes. */
interface IClashRuntime {
    Bundle snapshot();
    Bundle logsAfter(long sequence);
    boolean acknowledgeTunReset();
    String groupsSnapshot();
    String outboundsSnapshot();
    String connectionsSnapshot();
    boolean selectOutbound(String group, String name);
    boolean urlTest(String group);
    boolean setClashMode(String mode);
    boolean closeConnection(String id);
    boolean closeAllConnections();
    boolean stopRuntime();
}
