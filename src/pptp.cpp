module clashflux.pptp;

import std;
import clashflux.vpn;

namespace pptp {

vpn::EngineAdapter MakePptpAdapter() {
    vpn::EngineAdapter adapter{
        .descriptor = vpn::EngineDescriptor{
            .kind = vpn::EngineKind::SystemPptp,
            .priority = 80,
            .available = false,
            .connectionKinds = {vpn::ConnectionKind::Pptp},
        },
    };
    adapter.connect = [](vpn::VpnConnection&, std::string& error) {
        error = "移动端不支持桌面 PPTP 系统拨号";
        return false;
    };
    return adapter;
}

} // namespace pptp
