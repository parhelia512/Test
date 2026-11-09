// core_store_lifecycle.inc — sing-box 内核生命周期
// 由 src/store/core_store.cppm 在 CoreStore 类体内包含。

    // Mobile runtimes are packaged with the application; no desktop daemon
    // needs installation, service IPC or a separate upgrade handoff.
    bool startReadyCore() {
        std::lock_guard operationLock(lifecycleMutex_);
        ensureOpen();
        if (snapshot().state == core::CoreState::Running) return true;
        const auto profileYaml = currentMainContent();
        preparedOptions_ = routingOptions(profileYaml, false);
        preparedOptions_->proxyListening = false;
        startCore(profileYaml, false, false, false);
        return snapshot().state == core::CoreState::Running && snapshot().lastError.empty();
    }

    bool connectProxy() {
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
        ensureOpen();
        const auto before = snapshot();
        if (before.state == core::CoreState::Running && before.proxyListening) return true;
        // Read source and selected identity under the same lifecycle lock as compilation.
        const auto profileYaml = currentMainContent();
        auto options = routingOptions(profileYaml, tunEnabled());
        options.proxyListening = true;
        std::string error;
        auto candidate = prepareStartPlan(std::move(options), error);
        if (!candidate) {
            std::lock_guard lock(mutex_);
            snap_.lastError = "连接候选计划被拒绝：" + error;
            return false;
        }
        if (before.state == core::CoreState::Running) {
            if (!restartPreparedCore(profileYaml, std::move(candidate), before,
                    systemProxyEnabled(), false, {}, std::chrono::steady_clock::now())) return false;
        } else {
            preparedStart_ = std::move(candidate);
            startCore(profileYaml, false, tunEnabled());
        }
        return snapshot().state == core::CoreState::Running && snapshot().proxyListening &&
               snapshot().lastError.empty();
    }

    void startCore(const std::string& profileYaml, bool startDetached = false,
                   bool tunEnabled = false,
                   std::optional<bool> systemProxyState = std::nullopt,
                   bool speedTestOnly = false,
                   bool repairRuleSetCache = true,
                   bool startVpn = true) {
        std::lock_guard operationLock(lifecycleMutex_);
        // 任何未捕获异常都必须让状态离开 Starting：以前 json::parse /
        // filesystem 抛出会越过 fail()，把 snap_.state 永久留在 Starting，而
        // 「启动中拒绝重入」的保护会因此挡掉之后所有启动，直到重启应用。
        try {
            startCoreUnchecked(profileYaml, startDetached, tunEnabled,
                               systemProxyState, speedTestOnly,
                               repairRuleSetCache, startVpn);
        } catch (const std::exception& exception) {
            settleStartFailure(std::string{"内核启动异常："} + exception.what());
        } catch (...) {
            settleStartFailure("内核启动未知异常");
        }
        preparedStart_.reset();
        preparedOptions_.reset();
    }

    // 异常只记错误、不把已经跑起来的内核翻成 Failed；仍停在 Starting 时才走 fail()。
    void settleStartFailure(const std::string& error) {
        {
            std::lock_guard lock(mutex_);
            if (snap_.state != core::CoreState::Starting) {
                snap_.lastError = error;
                stream::logApplication("warning", "内核启动流程异常：" + error);
                return;
            }
        }
        fail(error);
    }

    void startCoreUnchecked(const std::string& profileYaml, bool startDetached = false,
                            bool tunEnabled = false,
                            std::optional<bool> systemProxyState = std::nullopt,
                            bool speedTestOnly = false,
                            bool repairRuleSetCache = true,
                            bool startVpn = true) {
        static_cast<void>(startDetached);
        static_cast<void>(systemProxyState);
        static_cast<void>(repairRuleSetCache);
#if defined(CLASHFLUX_IOS)
        tunEnabled = true;
#endif
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
        ensureOpen();
        stream::logApplication("info", "开始启动代理内核");
        {
            std::lock_guard lock(mutex_);
            // Android 桥线程在应用启动时自动拉起内核，启动慢时（拉远程
            // 规则集等）UI 侧再点启动会 spawn 第二个实例，抢不到控制
            // 端口/混合端口的那个以 exit 1 收场，把「内核启动后立即退出」误报
            // 给用户。启动进行中直接拒绝重入。
            if (snap_.state == core::CoreState::Starting) return;
            binaryPath_ = "sing-box libbox";
            snap_.state = core::CoreState::Starting;
            snap_.runningSince = {};
            snap_.lastError.clear();
        }
        lastProfileYaml_ = profileYaml;

        const std::filesystem::path workDir = cfg::coreWorkDir();
        const std::filesystem::path configFile = workDir / "config.json";
        {
            auto plan = std::move(preparedStart_);
            std::string prepareError;
            if (!plan) {
                auto options = preparedOptions_ ? std::move(*preparedOptions_) :
                    routingOptions(profileYaml, tunEnabled, speedTestOnly);
                preparedOptions_.reset();
                plan = prepareStartPlan(std::move(options), prepareError);
            }
            if (!plan) { fail("规则集/订阅准备失败：" + prepareError); return; }
            auto& options = plan->options;
            auto& compiled = plan->compiled;
            auto& config = plan->config;
            // Re-read after preparation: another client may have started while
            // downloads/checks were running. Reject before replacing config.json.
            if (!checkNetworkConflicts(options, config, prepareError)) { fail(prepareError); return; }
            std::string commitError;
#if defined(__ANDROID__)
            if (!publishAndroidModeIntent(mode(), commitError)) { fail(commitError); return; }
#endif
            if (!api::CommitFile(plan->file, configFile, commitError)) { fail(commitError); return; }
            lastPreparedOptions_ = options;
            lastPreparedJson_ = compiled.json;
            lastPreparedRulePlan_ = std::make_shared<const clashflux::connection_trace::Plan>(std::move(compiled.rulePlan));
            cacheProxyGroups(options, compiled, std::move(plan->proxyGroups));
            std::lock_guard lock(mutex_);
            snap_.warnings = std::move(compiled.warnings);
            snap_.fidelity = std::move(compiled.fidelity);
            snap_.sourceObjects = previewSourceObjects_;
            snap_.participatingSources = std::move(compiled.participatingSources);
        }

        // Commit the accepted config before notifying the mobile runtime.
#if defined(__ANDROID__)
        {
            std::lock_guard lock(mutex_);
            snap_.mode = mode();
            snap_.mixedPort = mixedPort();
            snap_.allowLan = false;
            snap_.logLevel = logLevel();
            snap_.tunEnabled = tunEnabled && startVpn;
            snap_.state = (tunEnabled && startVpn) || speedTestOnly
                              ? core::CoreState::Stopped : core::CoreState::Starting;
        }
        if (!speedTestOnly) {
            if (tunEnabled && startVpn) {
                clashflux_android_start_vpn();
            } else if (!clashflux_android_start_core()) {
                fail("无法启动 Android sing-box 内核服务");
            }
        }
        return;
#else
        if (!process_.start(binaryPath_, workDir, configFile)) {
            fail(process_.lastError());
            return;
        }
#endif

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!process_.running()) {
                fail("移动 VPN 服务未能启动内核");
                return;
            }
            if (const auto result = nativeApi_->runtime(); result.ok) {
                std::lock_guard lock(mutex_);
                snap_.version = result.version;
                snap_.nativeStartedAt = result.startedAt;
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        if (!ready) {
            std::string error = "移动 VPN 控制接口 30s 内未就绪";
            if (!stopCore()) error += "；启动失败后的清理未完成：" + snapshot().lastError;
            fail(std::move(error));
            return;
        }

        streams_.start(cfg::controllerWsUrl(), secret_, logLevel(), cfg::nativeControllerBaseUrl());
        {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Running;
            snap_.runningSince = std::chrono::steady_clock::now();
            lastAppliedOptions_ = lastPreparedOptions_;
            lastAppliedJson_ = lastPreparedJson_;
            snap_.appliedRulePlan = lastPreparedRulePlan_;
            snap_.proxyListening = lastAppliedOptions_ && lastAppliedOptions_->proxyListening;
            snap_.networkRevision = lastAppliedOptions_ ? lastAppliedOptions_->network.revision : 0;
            snap_.connectedSince = snap_.proxyListening ? std::chrono::steady_clock::now()
                                                       : std::chrono::steady_clock::time_point{};
            // The ready instance is running this accepted plan. sing-box's
            // compatibility /configs does not report TUN status; leaving the
            // default false here makes the next routing handoff remove TUN.
            clashflux::runtime_plan::Ready(snap_, lastAppliedOptions_ && lastAppliedOptions_->tunInbound,
                lastAppliedOptions_ ? lastAppliedOptions_->planRevision : 0);
            setSetting("routing.applied_revision", std::to_string(snap_.planRevision));
        }
        stream::logApplication("info", "代理内核控制器已就绪");
        refreshRuntime();
        captureHealthEvidence(false);
    }

    bool stopCore() {
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
        stream::logApplication("info", "请求停止代理内核");
#if defined(__ANDROID__)
        if (!clashflux_android_stop_vpn()) {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Failed;
            snap_.lastError = "Android 后台服务未确认内核退出";
            return false;
        }
#else
        process_.stop();
        if (process_.running()) {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Failed;
            snap_.lastError = "iOS Packet Tunnel 未确认退出";
            return false;
        }
#endif
        streams_.stop();
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Stopped;
        snap_.runningSince = {};
        snap_.version.clear();
        snap_.lastError.clear();
        clashflux::runtime_plan::Stopped(snap_);
        captureHealthEvidence(false);
        return true;
    }

    // 订阅/配置变更时重启当前内核。Android 的 libbox 由 VpnService 托管，
    // 但无 TUN 内核也要保持常驻；写完新配置后按当前 TUN 状态恢复对应服务。
    auto lockPlanLifecycle(std::stop_token cancel = {}) {
        std::unique_lock lock(lifecycleMutex_, std::defer_lock);
        if (!cancel.stop_possible()) { lock.lock(); return lock; }
        while (!cancel.stop_requested()) {
            if (lock.try_lock()) return lock;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return lock;
    }

    // One owned candidate serves validation, handoff, preview and feedback.
    // The caller keeps the lifecycle lock from source lookup through commit.
    bool applyMainProfile(std::int64_t id, const std::string& content, std::string& error,
                          std::stop_token cancel = {}, std::string* fidelitySummary = nullptr) {
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
        ensureOpen();
        error.clear();
        const auto began = std::chrono::steady_clock::now();
        const auto before = snapshot();
        const bool running = before.state == core::CoreState::Running;
        const bool proxy = running && systemProxyOurs();
        std::int64_t previous = 0;
        for (const auto& profile : db_->listProfiles()) if (profile.selected) previous = profile.id;
        bool selectedChanged = false;
        try {
            if (cancel.stop_requested()) return false;
            auto options = routingOptions(content, before.tunEnabled, false, nullptr, routing::ConnectionId(id));
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
            if (running) options.proxyListening = before.proxyListening;
#endif
            auto candidate = prepareStartPlan(std::move(options), error, cancel, running);
            if (!candidate || cancel.stop_requested() ||
                !validatePlanTargets(candidate->options, candidate->compiled, nullptr, error)) return false;
            const auto summary = singbox::FidelitySummary(candidate->compiled.fidelity);
            if (cancel.stop_requested()) return false;
            db_->setSelectedProfile(id);
            selectedChanged = true;
            invalidateProxyGroupsSnapshot();
            if (running) {
                if (!restartPreparedCore(content, std::move(candidate), before, proxy, false, cancel, began)) {
                    if (!cancel.stop_requested()) error = snapshot().lastError;
                    db_->setSelectedProfile(previous);
                    invalidateProxyGroupsSnapshot();
                    return false;
                }
            } else {
                cacheProxyGroups(candidate->options, candidate->compiled, std::move(candidate->proxyGroups));
            }
            if (fidelitySummary) *fidelitySummary = summary;
            return true;
        } catch (const std::exception& exception) {
            if (selectedChanged) {
                db_->setSelectedProfile(previous);
                invalidateProxyGroupsSnapshot();
            }
            error = exception.what();
            return false;
        }
    }

    // 候选检查不改变订阅数据库、运行态或组选项；HTTP 自有缓存只在转换与
    // 候选检查成功后提交。适用于 UI/CLI 的写入口。
    bool validateRoutingPlan(const std::string& profileYaml, std::string& error,
                             const vpn::VpnPolicy* policy = nullptr,
                             const std::optional<std::string>& main = std::nullopt,
                             std::stop_token cancel = {}) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        try {
            if (cancel.stop_requested()) return false;
            auto options = routingOptions(profileYaml, snapshot().tunEnabled, false, policy, main);
            singbox::CompileResult compiled;
            bool candidateChecked = false;
            if (!prepareHttpRuleProviders(options, compiled, error, policy, cancel, nullptr,
                    &candidateChecked) || cancel.stop_requested()) return false;
            if (candidateChecked) return true;
            if (!validatePlanTargets(options, compiled, policy, error)) return false;
            if (cancel.stop_requested()) return false;
            return checkCompiledPlan(compiled, error);
        } catch (const std::exception& e) { error = e.what(); return false; }
    }

    singbox::RuleSetLibrary ruleSetLibrary() {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        return routing::DecodeRuleSetLibrary(clashflux::persistence::persistence().setting("rules.resources"));
    }

    struct RuleSetUsage {
        std::string resourceId, ruleId, profileName;
        std::int64_t profileId = 0; // 0: a global rule.
        bool enabled = true;
        bool operator==(const RuleSetUsage&) const = default;
    };

    singbox::ExternalResourceCache externalResourceCache(const singbox::ExternalResource& resource) {
        std::lock_guard lock(lifecycleMutex_);
        return singbox::InspectExternalResourceCache(resource, cfg::coreWorkDir() / "rule-providers");
    }

    std::vector<RuleSetUsage> ruleSetUsage() {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        std::vector<RuleSetUsage> usage;
        const auto policy = routing::DecodePolicy(setting("vpn.global_policy", ""));
        for (const auto& rule : policy.rules)
            if (rule.match == vpn::MatchKind::RuleSet)
                usage.push_back({rule.pattern, rule.id, "", 0, rule.enabled});
        for (const auto& profile : db_->listProfiles()) {
            const auto edits = routing::DecodeProfileRules(setting(routing::ProfileRulesKey(profile.id), ""));
            for (const auto& rule : edits.rules)
                if (rule.match == "rule_set" && rule.operation != "final")
                    usage.push_back({rule.pattern, rule.id, profile.name, profile.id,
                                     rule.enabled && rule.operation != "disable"});
        }
        return usage;
    }

    // Shared library edits serialize with profile edits/refreshes. Validate the
    // full proposed library before publishing settings or touching a running core.
    bool saveRuleSetLibrary(singbox::RuleSetLibrary library, std::string& error) {
        std::lock_guard lock(lifecycleMutex_);
        error.clear();
        try {
            ensureOpen();
            if (!clashflux::persistence::persistence().ready()) { error = "订阅尚未加载完成"; return false; }
            const auto previous = ruleSetLibrary();
            if (library.revision != previous.revision) { error = "规则集资源库已变化，请重新打开后编辑"; return false; }
            if (!singbox::ValidateRuleSetLibrary(library, error)) return false;
            const auto policy = routing::DecodePolicy(setting("vpn.global_policy", ""));
            if (!singbox::ValidateGlobalRuleSetReferences(library, policy.rules, error)) return false;
            for (const auto& use : ruleSetUsage())
                if (use.ruleId == "network-route" && std::ranges::none_of(library.resources,
                    [&](const auto& resource) { return resource.id == use.resourceId; })) {
                    error = "规则集仍被虚拟网卡接管设置引用：" + use.resourceId; return false;
                }
            // Even disabled personal records retain their references. Removal
            // cannot turn a later re-enable into a different or missing match.
            for (const auto& profile : db_->listProfiles()) {
                const auto edits = routing::DecodeProfileRules(setting(routing::ProfileRulesKey(profile.id), ""));
                if (!singbox::ValidateRuleSetReferences(library, edits, error)) { error += "：" + profile.name; return false; }
            }
            if (library.revision == std::numeric_limits<std::uint64_t>::max()) { error = "规则集修订号超出范围"; return false; }
            const auto content = currentMainContent();
            auto options = routingOptions(content, false);
            options.ruleSetLibrary = library;
            options.validateAllPersonalRuleSets = true;
            singbox::CompileResult compiled;
            bool checked = false;
            if (!prepareHttpRuleProviders(options, compiled, error, nullptr, {}, nullptr, &checked) ||
                (!checked && !checkCompiledPlan(compiled, error))) return false;
            ++library.revision;
            auto& data = clashflux::persistence::persistence();
            const auto oldText = data.setting("rules.resources");
            data.setSetting("rules.resources", routing::EncodeRuleSetLibrary(library));
            bool ok = true;
            try {
                if (snapshot().state == core::CoreState::Running) ok = restartCore(content, true);
            } catch (const std::exception& exception) { error = exception.what(); ok = false; }
            if (!ok) {
                data.setSetting("rules.resources", oldText);
                if (error.empty()) error = snapshot().lastError;
                if (error.empty()) error = "规则集未能应用";
                return false;
            }
            invalidateProxyGroupsSnapshot();
            return true;
        } catch (const std::exception& exception) { error = exception.what(); return false; }
    }

    // The pinned native control API has no resource-update RPC. Rotate only
    // this remote cache identity and use the existing checked restart/rollback.
    // A stopped/unreferenced resource is queued, never reported as downloaded.
    bool refreshPersonalRuleSet(const std::string& id, std::uint64_t expectedRevision,
                                bool& downloaded, std::string& error) {
        std::lock_guard lock(lifecycleMutex_);
        downloaded = false;
        error.clear();
        if (!core::SupportsRuleSetTools()) { error = "此平台暂不支持手动重新下载规则集"; return false; }
        try {
            auto library = ruleSetLibrary();
            if (library.revision != expectedRevision) { error = "规则集资源库已变化，请重新打开后编辑"; return false; }
            const auto resource = std::ranges::find(library.resources, id, &singbox::PersonalRuleSet::id);
            if (resource == library.resources.end() || resource->type != "remote") { error = "请选择远程规则集"; return false; }
            if (resource->refresh_generation == std::numeric_limits<std::uint64_t>::max()) { error = "规则集更新次数超出范围"; return false; }
            const auto options = routingOptions(currentMainContent(), tunEnabled());
            bool networkReferenced = false;
            const bool referenced = networkReferenced || std::ranges::any_of(options.globalRules, [&](const auto& rule) {
                return rule.enabled && rule.match == vpn::MatchKind::RuleSet && rule.pattern == id;
            }) || std::ranges::any_of(options.profileRuleEdits.rules, [&](const auto& rule) {
                return rule.enabled && rule.match == "rule_set" && rule.pattern == id &&
                    rule.operation != "disable" && rule.operation != "final";
            });
            const bool running = snapshot().state == core::CoreState::Running;
            ++resource->refresh_generation;
            if (!saveRuleSetLibrary(std::move(library), error)) return false;
            downloaded = running && referenced;
            return true;
        } catch (const std::exception& exception) { error = exception.what(); return false; }
    }

    singbox::CompileResult profileRulesPreview(std::int64_t id, const std::string& content) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        auto options = routingOptions(content, false, false, nullptr,
            routing::ConnectionId(id), nullptr, false);
        auto result = singbox::compileConfig(options);
        if (result.error.empty() && !result.json.empty()) {
            auto document = nlohmann::json::parse(result.json);
            // Editor previews do not need the application's control credentials.
            if (document.contains("experimental") && document["experimental"].contains("clash_api"))
                document["experimental"]["clash_api"].erase("secret");
            if (document.contains("services") && document["services"].is_array())
                for (auto& item : document["services"]) if (item.value("tag", "") == "clash-flux-control") item.erase("secret");
            result.json = document.dump(2);
        }
        return result;
    }

    bool validateProfileRuleSource(std::int64_t id, const std::string& content, std::string& error) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        auto options = routingOptions(content, false, false, nullptr,
            routing::ConnectionId(id), nullptr, false);
        singbox::CompileResult compiled;
        bool candidateChecked = false;
        if (!prepareHttpRuleProviders(options, compiled, error, nullptr, {}, nullptr,
                &candidateChecked)) return false;
        return candidateChecked || checkCompiledPlan(compiled, error);
    }

    std::string currentMainContent() {
        ensureOpen();
        const auto file = selectedProfileFile();
        if (file.empty()) return {};
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("无法读取 main 来源内容");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    bool sourceParticipates(std::int64_t id) {
        std::lock_guard operationLock(lifecycleMutex_);
        ensureOpen();
        const auto connection = routing::ConnectionId(id);
        if (std::ranges::any_of(nativeSessions_, [&](const auto& session) { return session.id == connection && session.connected; })) return true;
        for (const auto& profile : db_->listProfiles())
            if (profile.id == id && (profile.selected ||
                ((profile.type == "pptp") && !routing::SplitRoutes(profile.nativeRoutes).empty()))) return true;
        const auto policy = routing::DecodePolicy(setting("vpn.global_policy", ""));
        return std::ranges::any_of(policy.rules, [&](const auto& rule) {
            return rule.enabled && rule.connectionId == connection;
        });
    }

    bool standbyActive() {
#if defined(__ANDROID__)
        return clashflux::runtime_plan::Standby(snapshot(), clashflux_android_vpn_state(), tunEnabled());
#elif defined(CLASHFLUX_IOS)
        return false;
#else
        return clashflux::runtime_plan::Standby(snapshot());
#endif
    }

    // Called under the plan lock after temporarily clearing the main selection.
    // Android startup is asynchronous: a restart request cannot prove that the
    // old source was released. Require the background STOP acknowledgement.
    bool clearStandbyCore() {
#if defined(__ANDROID__)
        return stopCore();
#else
        return restartCore("");
#endif
    }

    bool sourceDeletionProtected(std::int64_t id) {
        std::lock_guard operationLock(lifecycleMutex_);
        ensureOpen();
        const auto connection = routing::ConnectionId(id);
        if (std::ranges::any_of(nativeSessions_, [&](const auto& session) { return session.id == connection && session.connected; })) return true;
        for (const auto& profile : db_->listProfiles())
            if (profile.id == id &&
                ((profile.selected && snapshot().state != core::CoreState::Stopped && !standbyActive()) ||
                 ((profile.type == "pptp") && !routing::SplitRoutes(profile.nativeRoutes).empty()))) return true;
        const auto policy = routing::DecodePolicy(setting("vpn.global_policy", ""));
        return std::ranges::any_of(policy.rules, [&](const auto& rule) {
            return rule.enabled && rule.connectionId == connection;
        });
    }

    bool validateSourceUpdate(std::int64_t id, const std::string& content, std::string& error,
                              bool forceProviderRefresh = false) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        try {
            const auto edits = routing::DecodeProfileRules(setting(routing::ProfileRulesKey(id), ""));
            if (!edits.rules.empty()) {
                const auto plan = singbox::InspectProfileRules(content, edits);
                if (!plan.error.empty()) { error = plan.error; return false; }
            }
            if (content.find_first_not_of(" \t\r\n") == std::string::npos) {
                error = "订阅内容为空"; return false;
            }
            const auto sourceId = routing::ConnectionId(id);
            const singbox::ProfileSource updated{sourceId, "", content};
            bool main = false;
            for (const auto& profile : db_->listProfiles()) if (profile.id == id && profile.selected) main = true;
            auto options = routingOptions(main ? content : currentMainContent(), snapshot().tunEnabled,
                                          false, nullptr, std::nullopt, &updated);
            if (forceProviderRefresh) options.forceProviderRefreshSources.insert(sourceId);
            if (sourceId == options.mainConnectionId) options.profileYaml = content;
            else {
                bool replaced = false;
                for (auto& source : options.auxiliarySources)
                    if (source.id == sourceId) { source.content = content; replaced = true; }
                if (!replaced) {
                    options.profileYaml = content; options.mainConnectionId = sourceId;
                    options.profileRuleEdits = edits;
                    options.auxiliarySources.clear(); options.globalRules.clear(); options.nativeConnections.clear();
                }
            }
            singbox::CompileResult compiled;
            bool candidateChecked = false;
            if (!prepareHttpRuleProviders(options, compiled, error, nullptr, {}, nullptr,
                    &candidateChecked)) return false;
            return candidateChecked || checkCompiledPlan(compiled, error);
        } catch (const std::exception& e) { error = e.what(); return false; }
    }

    // Resource actions never replace the subscription source. A late response
    // must still match the exact declaration/content captured by the page.
    bool refreshExternalResource(std::int64_t profileId, const std::string& expectedContent,
                                 const singbox::ExternalResource& selected,
                                 bool& applied, std::string& error, std::stop_token cancel = {}) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        applied = false;
        error.clear();
        try {
            const auto profiles = db_->listProfiles();
            const auto profile = std::ranges::find(profiles, profileId, &db::Profile::id);
            if (profile == profiles.end()) { error = "订阅不存在"; return false; }
            const auto readSource = [&] {
                std::ifstream input(cfg::profilesDir() / profile->file, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(input), {});
            };
            const auto guard = [&](std::string& failure) {
                if (cancel.stop_requested()) { failure = "资源更新已取消"; return false; }
                if (readSource() == expectedContent) return true;
                failure = "订阅已变化，请重新打开外部资源";
                return false;
            };
            if (!guard(error)) return false;
            const auto sourceId = routing::ConnectionId(profileId);
            std::string scanError;
            const auto inventory = singbox::InspectExternalResources(expectedContent, scanError, sourceId);
            if (selected.cacheKey.empty() ||
                std::ranges::find(inventory, selected) == inventory.end()) {
                error = "资源声明已变化或不支持手动刷新"; return false;
            }
            const auto before = snapshot();
            const auto content = currentMainContent();
            auto options = routingOptions(content, before.tunEnabled);
            const bool participating = options.mainConnectionId == sourceId ||
                std::ranges::any_of(options.auxiliarySources, [&](const auto& source) { return source.id == sourceId; });
            if (!participating) {
                options = routingOptions(expectedContent, false, false, nullptr, sourceId, nullptr, false);
                options.auxiliarySources.clear(); options.globalRules.clear(); options.nativeConnections.clear();
            }
            options.refreshProviderKey = selected.cacheKey;
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
            if (participating) options.proxyListening = before.proxyListening;
#endif
            const std::filesystem::path cacheRoot = selected.kind == "proxy-provider"
                ? options.proxyProviderCacheDir : options.ruleProviderCacheDir;
            std::error_code ec;
            std::filesystem::create_directories(cacheRoot, ec);
            if (ec) { error = "无法创建资源缓存目录"; return false; }
            rule_provider_cache::StagingDirectory backup;
            backup.path = cacheRoot / (".stage-resource-" + cfg::randomSecret());
            if (!std::filesystem::create_directory(backup.path, ec)) {
                backup.path.clear(); error = "无法创建独占资源暂存目录"; return false;
            }
            const auto destination = cacheRoot / (selected.cacheKey + ".cache");
            const bool hadCache = std::filesystem::exists(destination);
            if (hadCache) {
                const auto stamp = std::filesystem::last_write_time(destination, ec);
                if (!ec) std::filesystem::copy_file(destination, backup.path / "previous", ec);
                if (!ec) std::filesystem::last_write_time(backup.path / "previous", stamp, ec);
            }
            if (ec) { error = "无法保留原资源缓存"; return false; }
            const auto restoreCache = [&] {
                if (hadCache) {
                    std::string restoreError;
                    if (!api::CommitFile(backup.path / "previous", destination, restoreError))
                        error += "；资源缓存恢复失败：" + restoreError;
                } else {
                    std::filesystem::remove(destination, ec);
                    if (ec) error += "；新资源缓存移除失败：" + ec.message();
                }
            };
            const auto began = std::chrono::steady_clock::now();
            auto plan = prepareStartPlan(std::move(options), error, cancel,
                participating && before.state == core::CoreState::Running, guard);
            if (!plan) { restoreCache(); return false; }
            if (!participating || before.state != core::CoreState::Running) {
                resourceRetries_.erase(selected.identity);
                return true;
            }
            if (restartPreparedCore(content, std::move(plan), before, systemProxyOurs(), true, cancel, began)) {
                resourceRetries_.erase(selected.identity);
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
                applied = true;
#endif
                return true;
            }
            error = snapshot().lastError;
            restoreCache();
            return false;
        } catch (const std::exception& exception) { error = exception.what(); return false; }
    }

    bool restartCore(const std::string& profileYaml, bool skipUnchanged = false,
                     std::stop_token cancel = {}) {
        std::lock_guard operationLock(lifecycleMutex_);
        if (cancel.stop_requested()) return false;
        auto healthPause = streams_.pauseHealth();
        const auto before = snapshot();
        const bool systemProxyEnabled = systemProxyOurs();
        const auto preparationBegan = std::chrono::steady_clock::now();
        std::unique_ptr<PreparedStart> candidate;
        std::string error;
        try {
            auto options = routingOptions(profileYaml, before.tunEnabled);
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
            options.proxyListening = before.proxyListening;
#endif
            candidate = prepareStartPlan(std::move(options), error, cancel);
            if (!candidate) {
                if (cancel.stop_requested()) return false;
                std::lock_guard lock(mutex_); snap_.lastError = "候选计划被拒绝：" + error; return false;
            }
        } catch (const std::exception& e) {
            if (cancel.stop_requested()) return false;
            std::lock_guard lock(mutex_); snap_.lastError = e.what(); return false;
        }
        return restartPreparedCore(profileYaml, std::move(candidate), before, systemProxyEnabled,
                                   skipUnchanged, cancel, preparationBegan);
    }

    // 拉 /configs 刷新运行配置快照（阻塞：UI 必须 RunOnTaskThread）。
    void refreshRuntime() {
        ensureOpen();
#if defined(__ANDROID__)
        // libbox has no Clash-compatible /configs endpoint.  Its VPN state
        // is reflected by snapshot(); configuration is persisted locally.
        return;
#else
        const auto r = api_->configs();
        if (!r.ok) return;
        const auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (!j.is_object()) return;
        std::lock_guard lock(mutex_);
        snap_.mode = j.value("mode", "");
        snap_.mixedPort = j.value("mixed-port", snap_.mixedPort);
        snap_.allowLan = j.value("allow-lan", snap_.allowLan);
        snap_.logLevel = j.value("log-level", snap_.logLevel);
        // TUN belongs to the accepted running plan and platform lifecycle,
        // rather than the incomplete Clash compatibility config response.
#endif
    }

    // 模式意图持久化供后台恢复，运行中通过 CommandClient 设置并等待回读。
    bool applyMode(const std::string& m) {
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
        ensureOpen();
        if (m != "rule" && m != "global" && m != "direct") return false;
        bool live = false;
#if defined(__ANDROID__)
        // 无 TUN 的常驻 libbox 也接受模式热切换；Starting 阶段先把最新
        // 意图交给后台，由启动流程在发布 Running 前协调并确认。
        const int runtimeState = clashflux_android_core_state();
        live = runtimeState == 1 || runtimeState == 2;
        const auto previous = mode();
        std::string intentError;
        if (!publishAndroidModeIntent(m, intentError)) {
            std::lock_guard lock(mutex_);
            snap_.lastError = intentError;
            return false;
        }
        if (live && !clashflux_android_set_clash_mode(m.c_str())) {
            if (!publishAndroidModeIntent(previous, intentError))
                stream::logApplication("error", "恢复 Android 模式意图失败：" + intentError);
            return false;
        }
#else
        {
            std::lock_guard lock(mutex_);
            live = snap_.state == core::CoreState::Running;
        }
        if (live) {
            const nlohmann::json body = {{"mode", m}};
            const auto r = api_->patchConfigs(body.dump());
            if (!r.ok) return false;
        }
#endif
        setSetting("core.mode", m);
        std::lock_guard lock(mutex_);
        snap_.mode = m;
        return true;
    }

    void checkAlive() {
#if defined(__ANDROID__)
        std::lock_guard lock(mutex_);
        if (clashflux_android_core_state() == 3) {
            snap_.state = core::CoreState::Failed;
        } else if (clashflux_android_core_state() == 2) {
            snap_.state = core::CoreState::Running;
        } else if (clashflux_android_core_state() == 1) {
            snap_.state = core::CoreState::Starting;
        } else if (snap_.state != core::CoreState::Failed) {
            snap_.state = core::CoreState::Stopped;
        }
        return;
#else
        std::unique_lock owner(lifecycleMutex_, std::try_to_lock);
        if (!owner.owns_lock()) return;
        std::lock_guard lock(mutex_);
        if (snap_.state != core::CoreState::Running) return;
        if (process_.running()) return;
        snap_.state = core::CoreState::Failed;
        snap_.lastError = "iOS Packet Tunnel 已停止";
#endif
    }
