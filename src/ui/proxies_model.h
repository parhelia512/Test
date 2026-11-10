// proxies_model.h — 策略组快照的唯一来源（跨页面共享的 application service）。
//
// 代理页、首页卡片、托盘菜单都只读这里的 State。快照只有一种来源：当前正在
// 运行的内核（Android 经 Binder 的 CommandClient，桌面经 clash_api）。内核未
// 运行、未连接或查询失败时快照为 Empty——UI 不保留上一轮结果，也不使用按订阅
// 编译的预览，避免把「看起来像实时」的旧数据当成当前状态。
//
// 只有一个拉取器（common.cpp 的 FetchProxiesSnapshot）与一个驱动循环
// （DriveProxiesModel，在 AppRoot 组合期启动一次，2s 一拍、有请求时立即补一拍），
// 结果写进这里的 State。State::Write 按 operator== 去重，内容没变不会通知订阅者。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "wire_codec.h"
#include "proxy_test_model.h"
#include "ui.h" // ProxyGroupSnapshot

namespace clashflux::ui {

struct CoreModel; // core_model.h（必须先导入 clashflux.store.core）

// 快照来源。Live = 本次从正在运行的内核查询成功；Empty = 内核未运行、会话
// 已失效或查询失败。UI 不使用订阅编译预览或上一轮结果回退。
enum class ProxiesSource {
    Empty,
    Live,
};

struct ProxiesSnapshot {
    bool loaded = false;                     // 首轮获取（成功或失败）已结束
    std::vector<ProxyGroupSnapshot> groups; // 扁平表示（首页卡片 / 托盘菜单）
    std::map<std::string, std::string> labels; // 稳定 tag → 来源限定的显示名
    std::string body;                       // 内容版本比较用的原文
    std::shared_ptr<const wire::Proxies> proxies; // 工作线程生成的不可变拥有型 DTO
    ProxiesSource source = ProxiesSource::Empty;
    // 产生本次结果的运行会话（Android 来自 CommandClient 查询；桌面为 0）。
    // 必须参与去重比较：内核重启后组内容可能逐字节相同，但旧快照已不属于
    // 当前会话，不能被 State 的相等去重留在页面上。
    std::uint64_t runtimeEpoch = 0;
    std::uint64_t clientGeneration = 0;
    // 本次查询失败时的诊断，不代表可显示的代理数据。
    std::string error;

    bool operator==(const ProxiesSnapshot& other) const {
        // DTO is a deterministic projection of body. Pointer identity changes
        // on each fetch and must not force State notifications for equal data.
        return loaded == other.loaded && labels == other.labels && groups == other.groups &&
               body == other.body && source == other.source &&
               runtimeEpoch == other.runtimeEpoch &&
               clientGeneration == other.clientGeneration &&
               error == other.error;
    }
};

// 应用级单例：在 application_hooks 里 Provide，组件用 UseService<ProxiesModel>()。
struct ProxiesModel {
    std::shared_ptr<ProxyTestModel> tests = std::make_shared<ProxyTestModel>();
    // 唯一的订阅点。读它即订阅：内容变化时读取方所在的部分重组。
    // 必须带初值：huxerui::State 的默认构造是「空 cell」，读取/写入都会抛
    // "Lifecycle dependency State is empty"。
    huxerui::State<ProxiesSnapshot> snapshot{ProxiesSnapshot{}};
    // 用户动作（切节点 / 测速 / 切换订阅）后自增：驱动循环看到计数变化就跳过
    // 本次等待、立即再拉一拍。动作方不需要知道数据怎么取。
    huxerui::State<std::uint64_t> refreshTick{0};

    void RequestRefresh() { refreshTick = refreshTick.Get() + 1; }
};

// 安装应用级 UI 模型（application_hooks 调用一次；定义在 common.cpp）。
// 与 app_http_client.h 的 InstallAppHttpClient 同一套写法。
void InstallClashFluxUiModels(huxerui::ApplicationContext& context);

// 在 worker 线程上取一次快照（纯阻塞 IO + 解析，不触碰 State）。
// runtimeEpoch/clientGeneration 是调用方在 UI 线程读到的当前内核会话；返回体
// 携带的会话不一致（内核已重启/控制连接已更换）时结果为 Empty。
ProxiesSnapshot FetchProxiesSnapshot(std::uint64_t runtimeEpoch,
                                     std::uint64_t clientGeneration);

// 启动唯一的数据泵（组合期调用一次）。写入恒在 UI 线程，内容相等时去重。
// 内核不在 Running 时不发起查询，并立即发布 Empty 快照实现失效。
void DriveProxiesModel(huxerui::TaskScope tasks,
                       std::shared_ptr<ProxiesModel> model,
                       huxerui::State<std::size_t> pagerPage,
                       std::shared_ptr<CoreModel> coreModel);

} // namespace clashflux::ui
