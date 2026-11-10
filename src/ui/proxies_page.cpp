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

// Group and member lookup stays on the immutable live wire DTO.
bool isGroupType(const std::string& type) {
    return type == "Selector" || type == "selector" || type == "URLTest" ||
           type == "urltest" || type == "Fallback" || type == "fallback" ||
           type == "LoadBalance" || type == "loadbalance" || type == "Relay" ||
           type == "relay";
}

bool isSelectorType(const std::string& type) {
    return type == "Selector" || type == "selector";
}

const wire::Proxy* findGroup(const wire::Proxies& proxies, std::string_view tag) {
    const auto found = proxies.entries.find(std::string(tag));
    return found != proxies.entries.end() && isGroupType(found->second.type) &&
                   found->second.members
        ? &found->second : nullptr;
}

std::string displayName(std::string_view tag,
                        const std::map<std::string, std::string>& labels) {
    const auto found = labels.find(std::string(tag));
    return found == labels.end() ? std::string(tag) : found->second;
}

std::vector<std::string> resolvePath(
    const wire::Proxies& proxies, const std::vector<std::string>& raw) {
    std::vector<std::string> path;
    if (raw.empty() || !findGroup(proxies, raw.front())) return path;
    path.push_back(raw.front());
    const wire::Proxy* parent = findGroup(proxies, raw.front());
    for (std::size_t i = 1; parent && parent->members && i < raw.size(); ++i) {
        const auto member = std::ranges::find(*parent->members, raw[i]);
        if (member == parent->members->end() || !findGroup(proxies, *member)) break;
        path.push_back(raw[i]);
        parent = findGroup(proxies, raw[i]);
    }
    return path;
}

bool IsLiveProxySnapshot(const ProxiesSnapshot& snapshot,
                         const store::CoreSnapshot& core) {
    if (snapshot.source != ProxiesSource::Live || !snapshot.proxies ||
        core.state != core::CoreState::Running) return false;
#if defined(__ANDROID__)
    return core.binderAvailable && snapshot.runtimeEpoch == core.runtimeEpoch &&
           snapshot.clientGeneration == core.clientGeneration;
#else
    return true;
#endif
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

// NodeCard reads protocol, delay, and UDP from the live wire entry.
constexpr float kProxyNodeWidth = 196.0F;
constexpr float kSpeedFabFooterHeight = 80.0F;
constexpr std::size_t kNodeMetaChars = 8;

[[huxerui::composable]] huxerui::View NodeCard(
    std::string nodeDisplayName, const wire::Proxy* node, bool isGroup,
    bool selected, bool selectable, std::function<void()> onActivate) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto tests = huxerui::UseService<ProxiesModel>()->tests;
    static_cast<void>(tests->generation.Get());
    const bool testing = tests->progress.Get().busy;
    const int delay = node ? node->delay : 0;
    const std::string_view type = node ? std::string_view(node->type) : std::string_view{};
    const bool udp = node && node->udp;

    std::string meta;
    if (!isGroup && testing) meta = "测速中…";
    else if (!isGroup && delay > 0) meta = std::format("{} ms", delay);
    const std::string localizedMeta =
        meta == "测速中…" ? huxerui::UseString(Localized("测速中…")) : meta;
    const std::string metaText = localizedMeta.size() > kNodeMetaChars
                                     ? truncateOneLine(localizedMeta, kNodeMetaChars)
                                     : localizedMeta;
    const SelectableTileColors tile = ResolveSelectableTileColors(theme, selected);
    const huxerui::Color detailColor = selected ? tile.fg : tile.muted;
    const huxerui::TextStyle detailStyle{
        huxerui::Font::System(font_size::kCaption), detailColor};
    std::string detail(type);
    if (detail.empty()) {
        if (udp) detail = "UDP";
    } else if (!isGroup && udp) {
        detail += " · UDP";
    }
    const huxerui::View detailView = FittedSingleLineLabelFill(detail, detailStyle);
    const huxerui::Color metaColor =
        selected ? tile.fg
                 : (meta.empty() || meta == "测速中…"
                        ? tile.muted
                        : delayColor(theme, delay, false));
    huxerui::View metaView =
        metaText.empty()
            ? huxerui::View{huxerui::Row{}.With(
                  huxerui::Frame{.width = kProxyDelaySlotWidth})}
            : huxerui::View{
                  huxerui::Row{huxerui::Text(metaText).Style(
                      huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption), metaColor})}
                      .With(huxerui::Frame{.width = kProxyDelaySlotWidth},
                            huxerui::MainAlign(huxerui::MainAxisAlignment::End))};
    return ProxyNodeTile(NodeTitle(nodeDisplayName, tile.fg), detailView,
        std::move(metaView), selected, nodeDisplayName, std::move(onActivate),
        selectable ? SelectableTileInteraction::Selectable : SelectableTileInteraction::ReadOnly,
        false, testing, tests->progress.Get().busy, [] {});
}

std::function<void()> NodeSelectAction(
    huxerui::TaskScope tasks, std::shared_ptr<ProxiesModel> proxiesModel,
    std::function<void()> onFailure, std::function<void()> onReadOnly,
    std::string groupName, std::string nodeName, bool selectable) {
    if (!selectable) return onReadOnly;
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
    auto mode = huxerui::UseState<std::string>("rule");
    auto proxySearch = huxerui::UseState(huxerui::TextEditingValue{});
    auto sectionMotion = UseSectionTabMotion();
    const auto testTasks = huxerui::UseApplicationTaskScope();
    const auto testModel = proxiesModel->tests;
    const auto testProgress = testModel->progress;
    // Path state stores group tags and is validated against the current snapshot.
    auto rulePath = huxerui::UseState<std::vector<std::string>>({});
    auto globalPath = huxerui::UseState<std::vector<std::string>>({});
    // The page consumes the immutable wire DTO without building proxy node/group copies.
    huxerui::Lifecycle(
        [mode, proxiesModel, coreModel, testModel] {
            const store::CoreSnapshot core = coreModel->view.Get().core;
            testModel->ObserveContext(core.state == core::CoreState::Running,
                                      core.binderAvailable, core.planRevision,
                                      core.runtimeEpoch, core.clientGeneration);
            mode = coreModel->view.Get().Mode();
            static_cast<void>(proxiesModel->snapshot.Get());
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

    // Root group and path selection read the live wire snapshot directly.
    const ProxiesSnapshot& liveSnapshot = proxiesModel->snapshot.Get();
    const store::CoreSnapshot coreNow = coreModel->view.Get().core;
    const bool live = IsLiveProxySnapshot(liveSnapshot, coreNow);
    const bool direct = live && mode.Get() == "direct";
    const bool global = live && mode.Get() == "global";
    const std::shared_ptr<const wire::Proxies> proxyData = liveSnapshot.proxies;
    const wire::Proxies emptyProxies;
    const wire::Proxies& proxies = proxyData ? *proxyData : emptyProxies;
    const auto& entries = proxies.entries;
    const auto& labels = liveSnapshot.labels;

    std::vector<std::string> rootGroups;
    if (live && global) {
        const wire::Proxy* globalGroup = findGroup(proxies, "GLOBAL");
        const wire::Proxy* globalRoot = nullptr;
        std::string globalRootTag;
        if (globalGroup && !globalGroup->now.empty() && globalGroup->now != "GLOBAL") {
            globalRoot = findGroup(proxies, globalGroup->now);
            if (globalRoot) globalRootTag = globalGroup->now;
        }
        if (!globalRoot && globalGroup) {
            globalRoot = globalGroup;
            globalRootTag = "GLOBAL";
        }
        if (!globalRoot) {
            for (const auto& [tag, value] : entries) {
                if (tag != "GLOBAL" && isGroupType(value.type) && value.members &&
                    value.selectable.value_or(isSelectorType(value.type))) {
                    globalRoot = &value;
                    globalRootTag = tag;
                    break;
                }
            }
        }
        if (!globalRoot) {
            for (const auto& [tag, value] : entries) {
                if (tag != "GLOBAL" && isGroupType(value.type) && value.members) {
                    globalRoot = &value;
                    globalRootTag = tag;
                    break;
                }
            }
        }
        if (globalRoot) rootGroups.push_back(std::move(globalRootTag));
    } else if (live && !direct) {
        for (const auto& [tag, value] : entries) {
            if (tag != "GLOBAL" && isGroupType(value.type) && value.members)
                rootGroups.push_back(tag);
        }
    }

    std::vector<std::string> tabNames = rootGroups;
    huxerui::State<std::vector<std::string>> activePath = global ? globalPath : rulePath;
    std::vector<std::string> path = resolvePath(proxies, activePath.Get());
    if (path.empty() || std::ranges::find(rootGroups, path.front()) == rootGroups.end())
        path = rootGroups.empty() ? std::vector<std::string>{}
                                  : std::vector<std::string>{rootGroups.front()};
    const std::string selectedRoot = path.empty() ? std::string{} : path.front();
    const std::string currentGroupTag = path.empty() ? std::string{} : path.back();
    const wire::Proxy* currentGroup = findGroup(proxies, currentGroupTag);
    std::size_t selectedTab = rootGroups.size();
    for (std::size_t i = 0; i < rootGroups.size(); ++i)
        if (rootGroups[i] == selectedRoot) selectedTab = i;

    const std::function<void(const std::string&)> selectGroup =
        [activePath, rootGroups, selectedRoot](const std::string& name) {
            const auto& latestPath = activePath.Get();
            const std::string from = !latestPath.empty() &&
                    std::ranges::find(rootGroups, latestPath.front()) != rootGroups.end()
                ? latestPath.front() : selectedRoot;
            if (from == name || std::ranges::find(rootGroups, name) == rootGroups.end()) return;
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
    const bool showSpeedFab = !direct && currentGroup != nullptr;
    std::vector<huxerui::View> groupPages;
    groupPages.reserve(rootGroups.size());
    for (std::size_t page = 0; page < rootGroups.size(); ++page) {
        const std::string rootGroupTag = rootGroups[page];
        const std::string contentGroupTag = page == selectedTab && currentGroup
            ? currentGroupTag : rootGroupTag;
        const wire::Proxy* contentGroup = findGroup(proxies, contentGroupTag);
        if (!contentGroup || !contentGroup->members) continue;

        const auto& members = *contentGroup->members;
        const std::string query = proxySearch.Get().text;
        std::vector<std::size_t> visibleNodes;
        visibleNodes.reserve(members.size());
        for (std::size_t i = 0; i < members.size(); ++i) {
            const std::string& tag = members[i];
            const auto entry = entries.find(tag);
            const std::string_view type = entry == entries.end()
                ? std::string_view{} : std::string_view(entry->second.type);
            const bool groupNode = entry != entries.end() && isGroupType(entry->second.type);
            const bool udp = entry != entries.end() && entry->second.udp;
            std::string searchableType(type);
            if (searchableType.empty()) {
                if (udp) searchableType = "UDP";
            } else if (!groupNode && udp) {
                searchableType += " · UDP";
            }
            if (SearchTextMatches(tag, query) ||
                SearchTextMatches(displayName(tag, labels), query) ||
                SearchTextMatches(type, query) || SearchTextMatches(searchableType, query))
                visibleNodes.push_back(i);
        }
        const std::size_t nodeCount = visibleNodes.size();
        if (!query.empty() && nodeCount == 0) {
            groupPages.push_back(huxerui::Column{
                EmptyState(Localized("没有匹配的节点"), app::images::search),
            }.With(huxerui::Grow(1.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
             .Key("group-page-" + rootGroupTag));
            continue;
        }

        huxerui::View grid = PageVirtualGrid(
            nodeCount,
            [proxyData, labels, visibleNodes = std::move(visibleNodes),
             tasks, toast, proxiesModel, contentGroupTag, nodeCount](std::size_t index)
                -> huxerui::View {
                if (!proxyData || index >= nodeCount) return huxerui::View{};
                const wire::Proxy* group = findGroup(*proxyData, contentGroupTag);
                if (!group || !group->members || visibleNodes[index] >= group->members->size())
                    return huxerui::View{};
                const std::string tag = (*group->members)[visibleNodes[index]];
                const auto entry = proxyData->entries.find(tag);
                const wire::Proxy* node = entry == proxyData->entries.end()
                    ? nullptr : &entry->second;
                const bool groupNode = node && isGroupType(node->type);
                const bool selectable = group->selectable.value_or(isSelectorType(group->type));
                const std::string name = displayName(tag, labels);
                return NodeCard(name, node, groupNode, group->now == tag, selectable,
                    NodeSelectAction(tasks, proxiesModel,
                        [toast] { toast.Show(Localized("线路切换失败，请查看日志")); },
                        [toast] { toast.Show(Localized("此分组不支持手动切换节点，仅作展示")); },
                        contentGroupTag, tag, selectable))
                    .Key(contentGroupTag + "::" + tag);
            }, PageScrollInset(theme, true), kSectionCardSpacing,
            compact ? kCompactFloatingNavigationInset : (showSpeedFab ? kSpeedFabFooterHeight : 0.0F))
            .Columns(compact ? huxerui::GridColumns::Fixed(2)
                             : huxerui::GridColumns::Adaptive(kProxyNodeWidth))
            .EstimatedRowExtent(68.0F)
            .ColumnSpacing(kSectionCardSpacing)
            .With(huxerui::Grow(1.0F), huxerui::ScrollBar())
            .Key("group-grid-" + rootGroupTag);
        groupPages.push_back(huxerui::Column{grid}.With(
            huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("group-page-" + rootGroupTag));
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
            groupTabs.push_back(SectionTab{name, displayName(name, labels), {}});
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
    if (!direct && currentGroup != nullptr) {
        const std::string groupName = currentGroupTag;
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
