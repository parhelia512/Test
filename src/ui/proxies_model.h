// 代理页直接读原始 wire::Proxies；模型不缓存派生的组/节点列表。快照仅来自
// 当前运行内核，停止、会话失效或查询失败时 UI 不回退到预览或旧结果。
// 唯一拉取器由 common.cpp 的 FetchProxiesSnapshot 和 DriveProxiesModel 组成。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wire_codec.h"
#include "proxy_test_model.h"

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
    std::map<std::string, std::string> labels; // 稳定 tag → 当前运行内核的显示名
    std::string body;                       // 原始内容与快照 DTO 的版本比较
    // State 去重按 DTO 值比较，忽略 shared_ptr 地址。
    std::shared_ptr<const wire::Proxies> proxies;
    ProxiesSource source = ProxiesSource::Empty;
    // 产生本次结果的运行会话（Android 来自 CommandClient 查询；桌面为 0）。
    // 必须参与去重比较：内核重启后组内容可能逐字节相同，但旧快照已不属于
    // 当前会话，不能被 State 的相等去重留在页面上。
    std::uint64_t runtimeEpoch = 0;
    std::uint64_t clientGeneration = 0;
    // 本次查询失败时的诊断，不代表可显示的代理数据。
    std::string error;

    bool operator==(const ProxiesSnapshot& other) const {
        return loaded == other.loaded && labels == other.labels &&
               body == other.body && source == other.source &&
               ((!proxies && !other.proxies) ||
                (proxies && other.proxies && *proxies == *other.proxies)) &&
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
    // 2s poll or explicit action request skips the current wait and triggers a fresh query.
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
