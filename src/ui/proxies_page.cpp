// proxies_page.cpp — 代理页：分组标签栏 + 选中分组的节点卡网格。出站模式
// （规则/全局/直连）的切换入口只在首页「出站模式」卡片；本页只跟随内核当前
// 模式渲染对应内容，不重复提供切换按钮。
//
// 分组切换（标签栏）：一个根分组一个横向标签，纯文字、无边框无填充；选中项
// 高亮文字并在底部画一条主题色加粗指示线。标签过多时标签栏横向滚动；内容区
// 支持左右滑动手势切换相邻分组，两端继续滑动交给手机端 Pager 切一级页。
// 同一时刻只展示选中分组的节点。
//
// 规则 → 订阅自带分组标签；全局 → 内核默认出站（`route.final`）所在的那个真实
// 分组，sing-box 合成的只读 GLOBAL 只作兜底；直连 → 不展示订阅内容，只给提示。
//
// 规则/全局两套分支路径 State 独立（rulePath/globalPath），切换模式互不
// 覆盖对方的选择。GLOBAL 组只在全局模式出现，规则列表不含它；Android
// 等仅返回真实策略组的平台，则全局模式回落到第一个可选策略组。
//
// 节点网格：Compact(<600) 两列；桌面按窗口宽度排 1/2/3/4 列，轨道均分铺满
// 页面宽度。嵌套面包屑固定在网格上方，不参与节点滚动。
//
// 分支语义：点组类型节点 = 选中该分支到当前组（selectProxy）并进入浏览；
// 叶子节点 = 常规切换。展开组内出现嵌套时，面包屑行提供返回上级。路径 State
// 只存用户走出的链，渲染期经 resolvePath 对最新 groups 校验裁剪，订阅刷新
// 导致组消失时自动回落，无需泵写状态。
//
// 延迟测试：页面右下角统一一个悬浮测速按钮（测试当前分组），组头不再各自
// 挂按钮；Compact 悬浮导航之上留出内缩量。
//
// 数据流：只读 ProxiesModel 的 State（AppRoot 驱动的唯一来源），原文变化时才
// 到任务线程解析，只同步有变化的组。测速 / 切节点均走运行内核的 CommandClient
// 或桌面 API，所有阻塞调用在 worker 上执行；页面仅展示内核实时回写的数据。
#include <huxerui/huxerui.h>

#include "app_resources.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "proxies_model.h"
#include "ui.h"
#include "page_layout.h"
#include "page_loading.h"
#include "empty_state.h"
#include "search_text.h"
#include "task_bridge.h"
#include "proxy_node_tile.h"

#include "wire_codec.h"
import clashflux.core;
import clashflux.store.core;

// CoreView 含 store::CoreSnapshot，必须在模块导入之后（同 profiles_cache.h）。
#include "core_model.h"

namespace clashflux::ui {
namespace {

struct ProxyNode {
    std::string name;
    std::string displayName;
    std::string type;
    int delay = 0;      // 0 = 未测；只来自本次内核查询回写的 history/urlTestDelay
    bool udp = false;
    bool isGroup = false;      // 该节点本身是策略组（订阅里的 url-test 分支）
    // 卡片第二行的元数据（协议大写 [+ " · UDP" 仅当内核真给了 udp]）：
    // 在解析期算好，item 工厂里零字符串分配——虚拟列表滚动时每帧都建卡片。
    std::string detail;

    bool operator==(const ProxyNode&) const = default;
};

struct ProxyGroup {
    std::string name;
    std::string displayName;
    std::string type;   // Selector / URLTest / Fallback / LoadBalance / Relay
    std::string now;    // 当前选中节点
    bool selectable = false;
    std::vector<ProxyNode> nodes;

    bool operator==(const ProxyGroup&) const = default;
};

// 组类型：带 all 列表的才是策略组（Selector/URLTest/Fallback/LoadBalance/Relay），
// 其余（Direct/Reject/具体协议节点）不进组列表。
bool isGroupType(const std::string& t) {
    return t == "Selector" || t == "selector" || t == "URLTest" ||
           t == "urltest" || t == "Fallback" || t == "fallback" ||
           t == "LoadBalance" || t == "loadbalance" || t == "Relay" ||
           t == "relay";
}

bool isSelectorType(const std::string& t) {
    return t == "Selector" || t == "selector";
}

// 卡片第二行元数据（定义在 NodeCard 之前，声明放这里供解析期调用）。
std::string BuildNodeDetail(const ProxyNode& node);

bool IsLiveProxySnapshot(const ProxiesSnapshot& snapshot,
                         const store::CoreSnapshot& core) {
    if (snapshot.source != ProxiesSource::Live || !snapshot.proxies ||
        core.state != core::CoreState::Running) return false;
#if defined(__ANDROID__)
    return core.binderAvailable &&
           snapshot.runtimeEpoch == core.runtimeEpoch &&
           snapshot.clientGeneration == core.clientGeneration;
#else
    return true;
#endif
}

std::vector<ProxyGroup> parseProxies(const std::shared_ptr<const wire::Proxies>& snapshot,
    const std::map<std::string, std::string>& labels) {
    std::vector<ProxyGroup> groups;
    if (!snapshot) return groups;
    const auto& all = snapshot->entries;
    for (const auto& [name, value] : all) {
        if (!isGroupType(value.type) || !value.members) continue;
        ProxyGroup group;
        group.name = name;
        group.displayName = labels.contains(name) ? labels.at(name) : name;
        group.type = value.type;
        group.now = value.now;
        group.selectable = value.selectable.value_or(isSelectorType(value.type));
        for (const auto& member : *value.members) {
            ProxyNode node;
            node.name = member;
            node.displayName = labels.contains(member) ? labels.at(member) : member;
            if (const auto found = all.find(member); found != all.end()) {
                node.type = found->second.type;
                node.udp = found->second.udp;
                node.isGroup = isGroupType(node.type);
                node.delay = found->second.delay;
            }
            node.detail = BuildNodeDetail(node);
            group.nodes.push_back(std::move(node));
        }
        groups.push_back(std::move(group));
    }
    // GLOBAL 组太长且无意义时沉底（clash_api 兼容组，列出全部节点）。
    std::ranges::stable_sort(groups, [](const ProxyGroup& a, const ProxyGroup& b) {
        return (a.name == "GLOBAL") < (b.name == "GLOBAL");
    });
    return groups;
}

void SyncProxyGroups(huxerui::StateList<ProxyGroup> groups,
                     std::vector<ProxyGroup> next) {
    if (groups.Size() != next.size()) {
        ReplaceStateList(groups, std::move(next));
        return;
    }
    // StateList::Set skips equal values. Preserve unchanged rows and their mounted
    // card state instead of invalidating the whole page on each polling tick.
    for (std::size_t i = 0; i < next.size(); ++i) {
        groups.Set(i, std::move(next[i]));
    }
}

const ProxyGroup* findGroup(const huxerui::StateList<ProxyGroup>& groups,
                            const std::string& name) {
    for (const auto& g : groups) {
        if (g.name == name) return &g;
    }
    return nullptr;
}

// 渲染期校验：只保留 navPath 中仍然存在且逐级可达（组类型子节点）的前缀；
// 空输入或根组消失返回空（调用方回落第一个组）。
std::vector<std::string> resolvePath(
    const huxerui::StateList<ProxyGroup>& groups,
    const std::vector<std::string>& raw) {
    std::vector<std::string> out;
    if (raw.empty()) return out;
    const ProxyGroup* g = findGroup(groups, raw.front());
    if (!g) return out;
    out.push_back(g->name);
    for (std::size_t i = 1; i < raw.size(); ++i) {
        bool reachable = false;
        for (const auto& n : g->nodes) {
            if (n.name == raw[i] && n.isGroup) {
                reachable = true;
                break;
            }
        }
        if (!reachable) break;
        out.push_back(raw[i]);
        g = findGroup(groups, raw[i]);
        if (!g) break;
    }
    return out;
}

// 延迟着色：未测灰 / <300ms 绿 / <1000ms 琥珀 / 超时红。
huxerui::Color delayColor(const huxerui::ThemeSpec& theme, int delay, bool timeout) {
    return DelayLevelColor(theme, delay, timeout || delay <= 0);
}

struct FittedTwoLineProxyLabel {
    class Extension;
    std::string text;
    huxerui::TextStyle style;
    bool operator==(const FittedTwoLineProxyLabel&) const = default;
};

class FittedTwoLineProxyLabel::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const FittedTwoLineProxyLabel& value) {
        Update(node, value);
    }

    void Update(huxerui::ViewNode&, const FittedTwoLineProxyLabel& value) {
        value_ = value;
        width_ = -1.0F;
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node,
                                      huxerui::TextMeasurer& measurer) override {
        const float width = node.ContentBounds().width;
        if (width == width_) return PaintInvalidation::None;
        width_ = width;
        const auto codePointEnds = [](const std::string& text) {
            std::vector<std::size_t> ends{0};
            for (std::size_t index = 1; index <= text.size(); ++index) {
                if (index == text.size() ||
                    (static_cast<unsigned char>(text[index]) & 0xc0U) != 0x80U) {
                    ends.push_back(index);
                }
            }
            return ends;
        };
        const auto fit = [&](std::string_view source, bool ellipsis) {
            const std::string full(source);
            const std::string suffix = ellipsis ? "…" : "";
            if (measurer.MeasureText(full + suffix, value_.style).size.width <= width) {
                return std::pair{full, source.size()};
            }
            const auto ends = codePointEnds(full);
            std::size_t low = 0;
            std::size_t high = ends.size();
            while (low + 1 < high) {
                const std::size_t middle = (low + high) / 2;
                if (measurer.MeasureText(full.substr(0, ends[middle]) + suffix,
                                         value_.style).size.width <= width) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            return std::pair{full.substr(0, ends[low]) + suffix, ends[low]};
        };
        const auto first = fit(value_.text, false);
        const std::string_view rest(value_.text.data() + first.second,
                                    value_.text.size() - first.second);
        const auto second = rest.empty()
            ? std::pair<std::string, std::size_t>{" ", 0}
            : fit(rest, true);
        rendered_ = first.first + "\n" + second.first;
        return PaintInvalidation::Foreground;
    }

    void PaintAboveContent(const huxerui::ViewNode& node,
                           huxerui::PaintContext& paint) const override {
        const auto bounds = node.ContentBounds();
        paint.PushClip(bounds);
        paint.DrawText(bounds, rendered_, value_.style,
            {.align = huxerui::TextAlign::Leading, .wrap = huxerui::TextWrap::NoWrap});
        paint.PopClip();
    }

private:
    FittedTwoLineProxyLabel value_;
    float width_ = -1.0F;
    std::string rendered_;
};

std::string truncateOneLine(const std::string& s, std::size_t maxCodePoints) {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (count == maxCodePoints) return s.substr(0, i) + "…";
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        i += std::min(len, s.size() - i);
        ++count;
    }
    return s;
}

// Windows' bundled emoji font displays regional-indicator pairs as country
// letters. Draw the original UTF-8 name in a retained two-line label; its
// ellipsis budget is measured against the card's final content width.
huxerui::View NodeTitle(const std::string& name,
                        huxerui::Color foreground) {
    const huxerui::TextStyle style{
        huxerui::Font::System(font_size::kBodySmall), foreground};
    return huxerui::Row{}.With(
        huxerui::Frame{.height = 32.0F, .min_width = 0.0F},
        huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
        FittedTwoLineProxyLabel{name, style});
}

// 卡片第二行元数据：协议（大写）+ UDP 标记；组节点写「组 · 当前分支」。
// 只由 parseProxies 调用一次（结果存进 ProxyNode::detail）。
std::string BuildNodeDetail(const ProxyNode& node) {
    std::string type = node.type;
    for (char& c : type) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (type.empty()) return node.udp ? std::string{"UDP"} : std::string{};
    // 成员本身是策略组时只显示组类型（URLTEST/SELECTOR…）：组没有"协议 + UDP"
    // 语义，也不该带「组 · 当前分支」这种旧分支文案。
    if (!node.isGroup && node.udp) type += " · UDP";
    return type;
}

// 节点卡自适应列的最小宽度：值越小同宽窗口下卡片越窄（列数更多）。
// 196 是"满屏 7 列"的阈值：1560 逻辑宽的满屏窗口去掉侧栏与岛内边距后约
// 1428 逻辑 px，1428 / (196 + 8) ≈ 7.0 → 7 列（每张约 197 px）；更宽的窗口
// 列数继续增加，更窄时逐级回落（卡片文本截断省略，窄列不溢出）。
constexpr float kProxyNodeWidth = 196.0F;

// 桌面右下角悬浮测速按钮的滚动尾部净空：56pt 按钮 + 24pt 底部外边距。
constexpr float kSpeedFabFooterHeight = 80.0F;

// sing-box CommandClient owns URLTest and its configured timeout.
constexpr std::size_t kNodeMetaChars = 8;

// 节点矩形卡：名称占两行，底部元数据左侧可收缩，右侧固定为测速槽。
// （组类型节点写「组·分支当前选中」），延迟按区间着色；选中态 primary 底。
// 宽度由 VirtualGrid 均分，高度由 EstimatedRowExtent 提供估计。
[[huxerui::composable]] huxerui::View NodeCard(
    const ProxyNode& node, bool selected,
    bool selectable, std::function<void()> onActivate) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto tests = huxerui::UseService<ProxiesModel>()->tests;
    static_cast<void>(tests->generation.Get()); // Rebind after a runtime-plan change.
    const bool testing = tests->progress.Get().busy;
    const int delay = node.delay;

    // 延迟只来自本次 CommandClient 快照；UI 不保留上一轮测速结果，也不自行生成超时。
    std::string meta;
    if (!node.isGroup && testing) {
        meta = "测速中…";
    } else if (!node.isGroup && delay > 0) {
        meta = std::format("{} ms", delay);
    }
    const std::string detail = node.detail;
    const std::string localizedMeta =
        meta == "测速中…" ? huxerui::UseString(Localized("测速中…")) : meta;
    const std::string metaText = localizedMeta.size() > kNodeMetaChars
                                     ? truncateOneLine(localizedMeta, kNodeMetaChars)
                                     : localizedMeta;

    const SelectableTileColors tile =
        ResolveSelectableTileColors(theme, selected);
    const huxerui::Color detailColor = selected ? tile.fg : tile.muted;
    const huxerui::TextStyle detailStyle{
        huxerui::Font::System(font_size::kCaption), detailColor};
    const huxerui::View detailView =
        FittedSingleLineLabelFill(detail, detailStyle);
    const huxerui::Color metaColor =
        selected ? tile.fg
                 : (meta.empty() || meta == "测速中…"
                        ? tile.muted
                        : delayColor(theme, delay, false));
    // 延迟槽固定宽度（含"测速中…"），内容**右对齐**贴住卡片右边距：以前 Text
    // 在槽内左对齐，短文本（"128 ms"）右侧会多出一截空隙，看着像没对齐；未测速
    // 时留空占位，卡片之间仍然对齐。
    huxerui::View metaView =
        metaText.empty()
            ? huxerui::View{huxerui::Row{}.With(
                  huxerui::Frame{.width = kProxyDelaySlotWidth})}
            : huxerui::View{
                  huxerui::Row{huxerui::Text(metaText).Style(
                      huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          metaColor})}
                      .With(huxerui::Frame{.width = kProxyDelaySlotWidth},
                            huxerui::MainAlign(
                                huxerui::MainAxisAlignment::End))};
    // 两行：第一行名称，第二行是「元数据（左）＋ 测速（右）」同一基线左右对齐。
    // 表面 / 内边距 / 圆角 / 交互全部交给 SelectableTile——代理页节点卡就是这套
    // 形状的标准（订阅卡等其它列表项同样复用它）。
    // 组测速入口统一在页面右下角的悬浮按钮；节点卡右侧只显示最近一次内核
    // URLTest 回写的延迟，不提供单节点测速、也不保存任何测速结果。
    return ProxyNodeTile(NodeTitle(node.displayName, tile.fg),
        detailView,
        std::move(metaView), selected, node.displayName, std::move(onActivate),
        selectable ? SelectableTileInteraction::Selectable : SelectableTileInteraction::ReadOnly,
        false, testing,
        tests->progress.Get().busy, [] {});
}

// 节点点击动作：**所有成员都只是"选中当前线路"**。成员本身是策略组时（订阅里
// 的 url-test 组，如「自动选择」）表示"这条线路交给它自动挑最快"，与 Clash
// Verge / metacubexd 一致：没有"点进去看子组"这一层，子组是标签栏里的独立分组。
// 切换写在任务协程里（点击节点可能随列表换组卸载）。
std::function<void()> NodeSelectAction(
    huxerui::TaskScope tasks, std::shared_ptr<ProxiesModel> proxiesModel,
    std::function<void()> onFailure, std::function<void()> onReadOnly,
    const ProxyGroup& group, const ProxyNode& node) {
    const std::string groupName = group.name;
    const std::string nodeName = node.name;
    if (!group.selectable) return onReadOnly;
    return [tasks, proxiesModel, onFailure, groupName, nodeName] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const bool selected = co_await RunOnTaskThread(
                [=] { return SelectProxyLine(groupName, nodeName); });
            if (!selected) {
                if (onFailure) onFailure();
                co_return;
            }
            proxiesModel->RequestRefresh();
        });
    };
}

} // namespace

[[huxerui::composable]] huxerui::View ProxiesPage(bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = UseFluxToast();
    // 策略组快照 / 内核状态的全局唯一来源（见 *_model.h）。
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    auto groups = huxerui::UseStateList<ProxyGroup>();
    auto mode = huxerui::UseState<std::string>("rule");
    auto proxySearch = huxerui::UseState(huxerui::TextEditingValue{});
    auto sectionMotion = UseSectionTabMotion();
    const auto testTasks = huxerui::UseApplicationTaskScope();
    const auto testModel = proxiesModel->tests;
    const auto testProgress = testModel->progress;
    // 分支路径按模式独立（互不共享）：规则模式 path[0] = 标签栏选中的订阅组，
    // 全局模式 path[0] = GLOBAL 或平台返回的实际可选组；后续元素 = 逐级点入的嵌套子组。
    auto rulePath = huxerui::UseState<std::vector<std::string>>({});
    auto globalPath = huxerui::UseState<std::vector<std::string>>({});
    // 已经投影过的快照原文：只在内容变化时重新投影（模型的 State 去重已保证
    // 通知只在内容变化时发生）。
    auto parsedLabels = huxerui::UseState<std::map<std::string, std::string>>({});
    auto parsedBody = huxerui::UseState<std::string>("");
    auto projectionGeneration = huxerui::UseState<std::uint64_t>(0);

    // 数据流完全由模型驱动（见 *_model.h）：内核状态、策略组快照任一变化才
    // 重新投影嵌套分组，**没有定时器**。唯一数据泵已完成 JSON 解码，
    // 页面在任务线程投影普通 DTO，UI 线程只发布模型。
    huxerui::Lifecycle(
        [tasks, groups, mode, proxiesModel, coreModel, testModel,
         parsedLabels, parsedBody, projectionGeneration] {
            const store::CoreSnapshot core = coreModel->view.Get().core;
            testModel->ObserveContext(core.state == core::CoreState::Running,
                                      core.binderAvailable, core.planRevision,
                                      core.runtimeEpoch, core.clientGeneration);
            // 页面只跟随内核**实际**出站模式；不再回落到保存的模式意图。
            mode = coreModel->view.Get().Mode();
            const ProxiesSnapshot& current = proxiesModel->snapshot.Get();
            if (current.body != parsedBody.Get() || current.labels != parsedLabels.Get()) {
                parsedLabels = current.labels;
                parsedBody = current.body;
                const auto ticket = projectionGeneration.Get() + 1;
                projectionGeneration = ticket;
                tasks.Launch([groups, projectionGeneration, ticket,
                              snapshot = current.proxies, labels = current.labels]() -> huxerui::Task<void> {
                    auto parsed = co_await RunOnTaskThread(
                        [snapshot = std::move(snapshot), labels = std::move(labels)] { return parseProxies(snapshot, labels); });
                    if (projectionGeneration.Get() != ticket) co_return;
                    SyncProxyGroups(groups, std::move(parsed));
                });
            }
            return [] {};
        },
        coreModel->view, proxiesModel->snapshot);

    // 组测速触发：Android 调用 libbox CommandClient.urlTest，UI 只消费内核回写；
    // 其它平台使用本地 API 实现。结果不会在 UI 中合成或缓存。
    const std::function<void(const std::string&)> triggerGroupTest =
        [testTasks, toast, testModel, coreModel, proxiesModel](std::string groupName) {
            testTasks.Launch([toast, testModel, coreModel, proxiesModel,
                              groupName = std::move(groupName)]() -> huxerui::Task<void> {
                if (testModel->progress.Get().busy) co_return;
                if (coreModel->view.Get().core.state != core::CoreState::Running) {
                    toast.Show(Localized("测速需要内核就绪，请检查内核服务"));
                    co_return;
                }
                const int generation = testModel->Begin();
                bool ok = false;
                try {
                    ok = co_await RunOnTaskThread([groupName] { return StartProxyGroupTest(groupName); });
                } catch (...) {
                }
                if (!testModel->Current(generation)) co_return;
                if (!ok) {
                    testModel->End(generation);
                    toast.Show(Localized("测速失败：内核未运行或启动失败"));
                    co_return;
                }
                proxiesModel->RequestRefresh();
                testModel->End(generation);
            });
        };

    // 按出站模式解析根分组与当前组（渲染期校验：组消失回落，嵌套前缀逐级
    // 校验）。直连不经过节点，不展示订阅组。快照不是 Live 时（内核停止、Binder
    // 断开、查询失败）连标签栏和悬浮测速也不渲染，避免旧投影残留在界面上。
    const huxerui::StateList<ProxyGroup> all = groups;
    const ProxiesSnapshot& liveSnapshot = proxiesModel->snapshot.Get();
    const store::CoreSnapshot coreNow = coreModel->view.Get().core;
    // Live 还需与**当前**运行会话一致：内核停止、Binder 断开或 epoch/代次变化
    // 后，快照即使还没被数据泵清掉也立即视为不可用，迟到结果不会重新上屏。
    const bool live = IsLiveProxySnapshot(liveSnapshot, coreNow);
    const bool direct = live && mode.Get() == "direct";
    const bool global = live && mode.Get() == "global";
    const ProxyGroup* syntheticGlobal = findGroup(all, "GLOBAL");
    // 全局模式的根组 = 内核**实际**走哪个出站。sing-box 的 GLOBAL 是 clash_api
    // 合成的只读组（type Fallback，`PUT /proxies/GLOBAL` 会 404），它的 `now`
    // 等于内核的默认出站（route.final = 订阅 MATCH 目标，缺省回落首个 selector 组）。
    // 直接拿 GLOBAL 当根组会让整屏卡片都点不动；随便挑一个组又会出现「改了不影响
    // 全局流量」的假象。所以按 `now` 找回那个真实组，只有找不回时才退回 GLOBAL。
    const ProxyGroup* globalRoot = nullptr;
    if (global) {
        const std::string globalTarget =
            syntheticGlobal != nullptr ? syntheticGlobal->now : std::string{};
        if (!globalTarget.empty()) {
            const ProxyGroup* target = findGroup(all, globalTarget);
            if (target != nullptr && target != syntheticGlobal) globalRoot = target;
        }
        if (globalRoot == nullptr) globalRoot = syntheticGlobal;
        if (globalRoot == nullptr) {
            for (const auto& g : all) {
                if (g.name != "GLOBAL" && g.selectable) {
                    globalRoot = &g;
                    break;
                }
            }
        }
        if (globalRoot == nullptr && !all.Empty()) {
            for (const auto& g : all) {
                if (g.name != "GLOBAL") {
                    globalRoot = &g;
                    break;
                }
            }
        }
    }

    // 标签栏列出订阅里的**每个**策略组（与 Clash Verge / metacubexd 一致）：
    // url-test 组（如订阅里的「自动选择」）也是独立分组——既能在父组里被选为
    // 线路（＝交给它自动挑最快），也能直接切到它自己的标签查看/测速，因此不再
    // 需要"点进子组"那一层。全局模式只保留 GLOBAL（或平台回落组），直连不走组。
    std::vector<const ProxyGroup*> rootGroups;
    if (!live) {
        // 非实时快照：不展示任何组、标签或节点。
    } else if (global) {
        if (globalRoot != nullptr) rootGroups.push_back(globalRoot);
    } else if (!direct) {
        for (const auto& g : all) {
            if (g.name == "GLOBAL") continue;
            rootGroups.push_back(&g);
        }
    }
    std::vector<std::string> tabNames;
    tabNames.reserve(rootGroups.size());
    for (const ProxyGroup* g : rootGroups) tabNames.push_back(g->name);

    // 活动路径：按模式取独立 State，渲染期校验（组消失回落，嵌套前缀逐级
    // 校验）。标签栏是分组入口：路径必须落在某个标签上，否则回落到首个标签；
    // 嵌套层级（path.size() > 1）由面包屑返回。
    huxerui::State<std::vector<std::string>> activePath =
        global ? globalPath : rulePath;
    std::vector<std::string> path = resolvePath(all, activePath.Get());
    const bool rootSelected =
        !path.empty() && std::ranges::any_of(rootGroups, [&](const ProxyGroup* g) {
            return g->name == path.front();
        });
    if (!rootSelected) {
        path = tabNames.empty() ? std::vector<std::string>{}
                                : std::vector<std::string>{tabNames.front()};
    }
    const ProxyGroup* current =
        path.empty() ? nullptr : findGroup(all, path.back());
    const std::string selectedRoot = path.empty() ? std::string{} : path.front();
    std::size_t selectedTab = tabNames.size();
    for (std::size_t i = 0; i < tabNames.size(); ++i) {
        if (tabNames[i] == selectedRoot) selectedTab = i;
    }

    // 分组标签与左右滑动直接更新路径，让选中态和内容在同一帧切换。
    const std::function<void(const std::string&)> selectGroup =
        [activePath, tabNames, selectedRoot](const std::string& name) {
            const auto& latestPath = activePath.Get();
            const std::string from = !latestPath.empty() &&
                                             std::ranges::find(tabNames, latestPath.front()) != tabNames.end()
                                         ? latestPath.front() : selectedRoot;
            if (from == name) return;
            const auto next = std::ranges::find(tabNames, name);
            if (next == tabNames.end()) return;
            activePath = std::vector<std::string>{name};
        };

    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 协议文本在卡片测量阶段按实际可用宽度省略，右侧测速槽固定占位。

    // 不可见时只保留本页 State/Lifecycle，不构建重子树：huxerui 的 Pager 会把
    // 四个一级页同时挂载，隐藏页即使不重组，其已挂载子树仍随每一帧被重新测量。
    // 真机实测（代理页大分组）：四页同挂时每帧 1443 次测量请求 / ~20ms，
    // 只留当前页后降到 28 次 / ~0ms；因此不可见页必须返回空占位。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("proxies-idle");

    if (!proxiesModel->snapshot.Get().loaded) {
        if (kPageTitlesInWindow && !compact)
            return PageLoadingScaffold(Localized("代理"), theme);
        return PageScaffold(Localized("代理"), huxerui::View{}, ProxyLoadingSkeleton(theme),
                            false, true, true);
    }

    // 一个根分组一页。VirtualGrid 直接用索引读取组节点，不再为所有组预先
    // 分配完整的 ProxyItem 与 span 数组；只有视口附近的节点会被构造为卡片。
    // 每个根组仅声明轻量虚拟网格；Pager 提供有界视口并只测量参与显示的页。
    // 不给各页叠加单独入场动画，避免与 Pager 的完整出入场轨道冲突。
    // 桌面悬浮测速按钮存在时，滚动尾部要留出它的净空，否则最后一行节点
    // 会被按钮遮住。
    const bool showSpeedFab = !direct && current != nullptr;
    std::vector<huxerui::View> groupPages;
    groupPages.reserve(rootGroups.size());
    for (std::size_t page = 0; page < rootGroups.size(); ++page) {
        const ProxyGroup& rootGroup = *rootGroups[page];
        // 只有当前页可能是嵌套子组（path 更深）；其余页就是各自的根分组。
        const bool selectedPage = page == selectedTab;
        const ProxyGroup* contentGroup =
            (selectedPage && current != nullptr) ? current : &rootGroup;

        const std::string contentGroupName = contentGroup->name;
        const std::string query = proxySearch.Get().text;
        std::vector<std::size_t> visibleNodes;
        for (std::size_t index = 0; index < contentGroup->nodes.size(); ++index) {
            const ProxyNode& node = contentGroup->nodes[index];
            if (SearchTextMatches(node.name, query) ||
                SearchTextMatches(node.displayName, query) ||
                SearchTextMatches(node.type, query) ||
                SearchTextMatches(node.detail, query)) {
                visibleNodes.push_back(index);
            }
        }
        const std::size_t nodeCount = visibleNodes.size();
        if (!query.empty() && nodeCount == 0) {
            groupPages.push_back(huxerui::Column{
                EmptyState(Localized("没有匹配的节点"), app::images::search),
            }.With(huxerui::Grow(1.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                 .Key("group-page-" + rootGroup.name));
            continue;
        }
        huxerui::View grid = PageVirtualGrid(
                                 nodeCount,
                                 [groups, visibleNodes = std::move(visibleNodes),
                                  tasks, toast, contentGroupName,
                                  nodeCount, compact, proxiesModel](std::size_t index)
                                     -> huxerui::View {
                                     const ProxyGroup* group =
                                         findGroup(groups, contentGroupName);
                                     if (group == nullptr || index >= nodeCount ||
                                         visibleNodes[index] >= group->nodes.size()) {
                                         return huxerui::View{};
                                     }
                                     const ProxyNode& node =
                                         group->nodes[visibleNodes[index]];
                                     const std::string nodeName = node.name;
                                     const std::string currentNow = group->now;
                                     return NodeCard(
                                                node, nodeName == currentNow,
                                                group->selectable,
                                                    NodeSelectAction(
                                                        tasks, proxiesModel,
                                                    [toast] {
                                                        toast.Show(
                                                            Localized("线路切换失败，请查看日志"));
                                                    },
                                                    [toast] {
                                                        toast.Show(Localized(
                                                            "此分组不支持手动切换节点，仅作展示"));
                                                    },
                                                    *group, node))
                                         .Key(group->name + "::" + nodeName);
                                 }, PageScrollInset(theme, true), kSectionCardSpacing,
                                 compact ? kCompactFloatingNavigationInset : (showSpeedFab ? kSpeedFabFooterHeight : 0.0F))
                                 .Columns(compact
                                              ? huxerui::GridColumns::Fixed(2)
                                              : huxerui::GridColumns::Adaptive(
                                                    kProxyNodeWidth))
                                 // 行高**只给估算值**，两行名称 + 元数据行 + 上下 8pt 内边距，
                                 // 真实高度仍由虚拟布局测量，避免字体缩放时裁切/错位。
                                 .EstimatedRowExtent(68.0F)
                                 .ColumnSpacing(kSectionCardSpacing)
                                 .With(huxerui::Grow(1.0F),
                                       huxerui::ScrollBar())
                                 .Key("group-grid-" + rootGroup.name);
        groupPages.push_back(huxerui::Column {grid}.With(
            huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("group-page-" + rootGroup.name));
    }

    huxerui::View body;
    if (!live) {
        // 内核停止、Binder 断开或查询失败：不展示任何缓存/预览，只给实时不可用提示。
        // Running 且首次查询在途时 loaded=false，走上面的骨架屏分支。
        body = EmptyState(Localized("代理内核未运行或查询失败，暂无实时数据"), app::images::proxies);
    } else if (direct) {
        body = EmptyState(Localized("直连模式 —— 流量不经过任何代理节点"), app::images::proxies);
    } else if (rootGroups.empty()) {
        body = EmptyState(Localized(global ? "全局模式暂无可用策略组"
                                           : "暂无策略组（检查订阅配置）"), app::images::proxies);
    } else {
        body = SectionTabPages(groupPages, selectedTab, [selectGroup, tabNames](std::size_t index) {
            if (index < tabNames.size()) selectGroup(tabNames[index]);
        }, sectionMotion);
    }

    // 标签栏固定在页面顶部（不随节点列表滚动），只有选中分组的节点参与滚动。
    std::vector<huxerui::View> columnChildren;
    if (!tabNames.empty()) {
        std::vector<SectionTab> groupTabs;
        groupTabs.reserve(tabNames.size());
        for (const std::string& name : tabNames) {
            groupTabs.push_back(SectionTab{
                name, findGroup(all, name) ? findGroup(all, name)->displayName : name,
                std::string{}});
        }
        columnChildren.push_back(
            SectionTabBar(groupTabs, selectedRoot, selectGroup, sectionMotion,
                          SectionTabPickerMode::ResponsiveGroups));
    }
    huxerui::View content = std::move(body);
    if (!columnChildren.empty()) {
        columnChildren.push_back(std::move(content).With(huxerui::Grow(1.0F)));
        content =
            huxerui::Column(std::move(columnChildren))
                .With(huxerui::Spacing(0.0F),
                      huxerui::Grow(1.0F),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Stretch));
    }

    // 出站模式切换入口在首页卡片；代理页标题行只保留节点搜索胶囊。
    huxerui::View searchField = PillSearchField(
        proxySearch, Localized("搜索节点"));
    huxerui::View page = PageScaffold(Localized("代理"), huxerui::View{},
                                      std::move(content), false, true, true,
                                      std::nullopt, {}, std::move(searchField));
    // 延迟测试按钮统一收在页面右下角：测试当前选中的分组。Compact 下要避开
    // 悬浮底部导航，桌面只留常规外边距。
    if (!direct && current != nullptr) {
        const std::string groupName = current->name;
        const GroupTestProgress progress = testProgress.Get();
        const bool testing = progress.busy;
        huxerui::View floatingSpeed =
            huxerui::IconButton(app::images::speed, Localized("测速"))
                .With(huxerui::Tooltip(Localized("测试当前分组延迟")),
                      huxerui::Frame{.width = 56.0F, .height = 56.0F},
                      huxerui::Enabled(!progress.busy),
                      huxerui::Opacity(testing ? 0.0F : 1.0F),
                      huxerui::Foreground(theme.colors.on_primary),
                      huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = huxerui::UseString(
                                             Localized("测速"))})
                .OnClick([triggerGroupTest, groupName] {
                    triggerGroupTest(groupName);
                });
        floatingSpeed = WithoutIconButtonOutlines(floatingSpeed);
        floatingSpeed = huxerui::Stack {
            std::move(floatingSpeed),
            testing ? huxerui::View{huxerui::ProgressCircle().With(
                          huxerui::Frame{.width = 28.0F, .height = 28.0F})}
                    : huxerui::View{huxerui::Row{}},
        }.With(huxerui::Frame{.width = 56.0F, .height = 56.0F},
               huxerui::Align(huxerui::HorizontalAlignment::Center,
                              huxerui::VerticalAlignment::Center),
               huxerui::Background(testing ? theme.colors.primary_container
                                           : theme.colors.primary),
               huxerui::Foreground(testing ? theme.colors.on_primary_container
                                           : theme.colors.on_primary),
               huxerui::CornerRadius(kFloatingActionCornerRadius),
               kPageTitlesInWindow ? ResolveFluxShadows(theme).floating_action
                   : huxerui::Shadow{ThemeShadowColor(theme, 0.28F), {}, 14.0F, 2.0F},
               huxerui::Scale(huxerui::AnimateTo(
                   testing ? 0.94F : 1.0F,
                   huxerui::TweenSpec{.duration = 0.18,
                                      .easing = huxerui::Easing::EaseOut})),
               huxerui::Semantics{.label = huxerui::UseString(
                   Localized(testing ? "测速中…" : "测速"))});
        // 与 Android 底部悬浮导航栏相同：按钮放在独立的全屏覆盖层中，
        // 由覆盖层的 Column 在主轴末端、交叉轴末端定位，不依赖页面内容容器。
        huxerui::View speedDock = huxerui::Column {
            std::move(floatingSpeed),
        }.With(huxerui::Padding(huxerui::EdgeInsets{
                   .right = theme.spacing.medium,
                   .bottom = compact ? kCompactFloatingNavigationInset
                                     : theme.spacing.large,
                   .left = theme.spacing.medium,
               }),
               huxerui::MainAlign(huxerui::MainAxisAlignment::End),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::End));
        return huxerui::Stack {
            std::move(page),
            std::move(speedDock),
        }.With(huxerui::Grow(1.0F),
               huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                              huxerui::VerticalAlignment::Stretch));
    }
    return page;
}

} // namespace clashflux::ui
