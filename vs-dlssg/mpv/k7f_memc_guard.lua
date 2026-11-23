--[[
k7f_memc_guard.lua —— 插帧跟不上时自动切回源帧率，避免卡成幻灯片

为什么需要它：
  mpv 的 --framedrop / --video-sync 只解决「音画不同步」，不减少 VapourSynth
  滤镜的计算量。滤镜算不过来时：
    - video-sync=audio + framedrop=vo：丢的是「显示时已过期」的帧，但每帧仍
      要等滤镜算完，画面会退化成低帧率幻灯片；
    - video-sync=display-resample：音频被拖慢去追视频，声音变慢/变调；
    - framedrop=decoder：在滤镜之前丢帧，RIFE/DLSSG 拿到不连续的源帧，插值
      会闪烁/鬼影，且破坏 vf_vapoursynth 的顺序请求假设。
  真正有效的做法是把插帧滤镜整个禁用，让 mpv 直接显示源帧。

判断依据（每 interval 秒采样一次，只在正常播放时统计）：
  lag      = 1 - (Δtime-pos / Δwall) / speed   —— 播放被拖慢的比例
  dropRate = Δframe-drop-count / Δwall         —— VO 每秒丢弃的过期帧
  mistRate = Δmistimed-frame-count / Δwall     —— display-sync 下未按时的帧
连续 enter_hold 秒超阈值 → `vf toggle @label` 禁用滤镜（不重建、不丢参数）；
保持 exit_hold 秒后试探性启用，若再超阈值则退避时间翻倍（上限 backoff_max）。
用标签切换 enable 标志，参数仍留在 vf 列表里，k7f_vpypar 不会被清空状态。

部署：
  1) 本文件放 portable_config\scripts\，与 k7f_vpypar.lua 一起用（滤镜需带标签，
     k7f_vpypar 的 apply 会自动加 @VDLSSG）。
  2) 可选 script-opts.conf：
       script-opts-append = k7f_memc_guard-enabled=yes
       script-opts-append = k7f_memc_guard-label=VDLSSG
       script-opts-append = k7f_memc_guard-script=~~/vs/MEMC_DLSSG_NV.vpy
  3) 可选按键（input_uosc.conf）：
       Ctrl+v  script-binding k7f_memc_guard/toggle
       v       script-binding k7f_memc_guard/status

脚本消息：
  script-message-to k7f_memc_guard on      打开守护并恢复插帧
  script-message-to k7f_memc_guard off     关闭守护并恢复插帧
  script-message-to k7f_memc_guard now     立即切回源帧率（测试用）
  script-message-to k7f_memc_guard status  OSD 显示当前状态
]]

local mp = require 'mp'
local msg = require 'mp.msg'

local o = {
    -- 总开关：false 时只提供 status/toggle，不自动降级
    enabled = true,
    -- 要控制的滤镜标签（k7f_vpypar 的 label），没有标签时按 script 文件名匹配
    label = 'VDLSSG',
    -- 无标签时的文件名匹配依据
    script = '~~/vs/MEMC_DLSSG_NV.vpy',

    interval = 0.5,          -- 采样间隔（秒）
    enter_lag = 0.08,        -- 播放速度低于 92% 视为跟不上
    enter_drop_rate = 5,     -- 每秒丢帧超过 5 帧视为跟不上
    enter_hold = 3,          -- 持续多少秒才降级
    start_grace = 3,         -- 起播/换片后多久才开始判定（引擎初始化会拖慢）
    source_grace = 2,        -- 降级后多久内不判「源帧仍卡」（切换瞬间要追进度）
    exit_hold = 30,          -- 首次降级后至少等多久才试探恢复
    probe_grace = 5,         -- 试探恢复后多久内不作判定（重建引擎会卡一下）
    backoff_max = 300,       -- 连续失败时退避上限
    osd = true,
}
require 'mp.options'.read_options(o, 'k7f_memc_guard')

local st = {
    last = nil,            -- { t, pos, drop, mist }
    spec = nil,            -- 无标签滤镜降级前的完整参数串，用于恢复
    played = 0,            -- 本片已正常播放的墙钟时长
    bad_time = 0,
    degraded = false,
    probing = false,
    probe_until = 0,
    next_probe = 0,
    backoff = 0,
    warned_source = false,
    last_ratio = 1,
    last_drop_rate = 0,
}

local function note(text, secs)
    msg.info(text)
    if o.osd then mp.osd_message(text, secs or 3) end
end

local function file_matches(a, b)
    if not a or not b then return false end
    local function norm(p)
        return (p:gsub('\\', '/'):match('[^/]+$') or p):lower()
    end
    return norm(a) == norm(b)
end

-- 找到插帧滤镜（含已被禁用的条目）
local function filter_entry()
    local list = mp.get_property_native('vf') or {}
    if o.label ~= '' then
        for _, f in ipairs(list) do
            if f.label == o.label then return f end
        end
    end
    for _, f in ipairs(list) do
        if f.name == 'vapoursynth' and f.params
            and file_matches(f.params.file, o.script) then
            return f
        end
    end
end

local function is_on(f)
    return f and f.enabled ~= false
end

-- 无标签滤镜无法用 @label 切换，只能按完整参数移除/重建
local function spec_of(f)
    if not (f and f.params and f.params.file) then return nil end
    local parts = { 'vapoursynth="' .. tostring(f.params.file) .. '"' }
    for k, v in pairs(f.params) do
        if k ~= 'file' then
            parts[#parts + 1] = k .. '="' .. tostring(v) .. '"'
        end
    end
    return table.concat(parts, ':')
end

-- 启用/禁用插帧滤镜。返回 false 表示状态没变成功
local function set_interp(on)
    local f = filter_entry()
    if not f then return false end
    if is_on(f) == on then return true end

    if f.label then
        local ok, ret = pcall(mp.commandv, 'vf', 'toggle', '@' .. f.label)
        if ok and ret == true then return true end
        msg.warn('vf toggle 失败：' .. tostring(ret))
        return false
    end

    if on then
        if not st.spec then return false end
        local ok, ret = pcall(mp.commandv, 'vf', 'append', st.spec)
        return ok and ret == true
    end

    st.spec = st.spec or spec_of(f)
    local ok, ret = pcall(mp.commandv, 'vf', 'remove',
        'vapoursynth="' .. tostring(f.params.file) .. '"')
    return ok and ret == true
end

local function reset_sampling()
    st.last = nil
    st.bad_time = 0
end

local function degrade(kind, force)
    if st.degraded and not force then return end
    if not filter_entry() then
        msg.warn('降级失败：找不到插帧滤镜')
        return
    end
    st.spec = st.spec or spec_of(filter_entry())
    if not set_interp(false) then
        msg.warn('降级失败：禁用插帧滤镜失败')
        return
    end

    st.degraded = true
    st.probing = false
    st.bad_time = 0
    st.warned_source = false
    -- 切换瞬间 time-pos 还在追赶滤镜的旧进度，必须重置基线再宽限几秒
    st.last = nil
    st.grace_until = mp.get_time() + o.source_grace
    st.backoff = (st.backoff == 0) and o.exit_hold
        or math.min(math.floor(st.backoff * 2), o.backoff_max)
    st.next_probe = mp.get_time() + st.backoff

    if kind == 'manual' then
        note('已手动切回源帧率', 3)
    else
        note(('插帧跟不上（速度 %.0f%% · 丢帧 %.1f/s），已切回源帧率'):format(
            (st.last_ratio or 1) * 100, st.last_drop_rate or 0), 4)
    end
end

local function start_probe()
    if not set_interp(true) then
        -- 滤镜条目被外部删掉了，保持降级，稍后再试
        st.next_probe = mp.get_time() + o.exit_hold
        return
    end
    st.probing = true
    st.probe_until = mp.get_time() + o.probe_grace
    st.bad_time = 0
    st.warned_source = false
    st.last = nil
    note(('重新启用补帧，观察 %d 秒…'):format(o.probe_grace), 2)
end

local function probe_ok()
    st.probing = false
    st.degraded = false
    st.backoff = 0
    st.bad_time = 0
    st.grace_until = nil
    note('补帧稳定，保持启用', 2)
end

local function sample()
    if not o.enabled then return end
    if mp.get_property_bool('paused', false)
        or mp.get_property_bool('core-idle', false)
        or mp.get_property_bool('seeking', false)
        or not mp.get_property_native('video') then
        reset_sampling()
        return
    end

    local f = filter_entry()
    if not f then
        reset_sampling()
        return
    end

    local t = mp.get_time()
    local pos = mp.get_property_number('time-pos')
    if not pos then
        reset_sampling()
        return
    end
    local drop = mp.get_property_number('frame-drop-count', 0) or 0
    local mist = mp.get_property_number('mistimed-frame-count', 0) or 0

    local last = st.last
    st.last = { t = t, pos = pos, drop = drop, mist = mist }
    if not last then return end

    local dt = t - last.t
    local dpos = pos - last.pos
    -- seek、缓冲空档、UI 卡顿都别计入
    if dt < 0.2 or dt > 3 or dpos < -0.05 then return end

    local speed = mp.get_property_number('speed', 1) or 1
    if speed <= 0 then speed = 1 end

    local ratio = (dpos / dt) / speed
    local drop_rate = math.max(0, (drop - last.drop) / dt)
    local mist_rate = math.max(0, (mist - last.mist) / dt)
    st.last_ratio, st.last_drop_rate = ratio, drop_rate
    st.played = st.played + dt

    -- 起播、降级切换、试探期内的抖动不算数（引擎重建会短暂卡顿）
    local in_grace = st.played < o.start_grace
        or (st.grace_until and t < st.grace_until)
        or (st.probing and t < st.probe_until)

    local bad = false
    if not in_grace then
        bad = ratio < (1 - o.enter_lag)
            or drop_rate > o.enter_drop_rate
            or mist_rate > o.enter_drop_rate
    end

    if st.degraded then
        if st.probing then
            if bad then
                st.bad_time = st.bad_time + dt
                if st.bad_time >= o.enter_hold then
                    degrade('again', true)
                end
            else
                -- 连续正常跑满 enter_hold 才算恢复成功，间歇抖动不算
                st.bad_time = 0
                if t >= st.probe_until + o.enter_hold then probe_ok() end
            end
        elseif t >= st.next_probe then
            start_probe()
        elseif bad and not st.warned_source then
            -- 已经关了补帧还卡：瓶颈不在插帧
            st.warned_source = true
            note('补帧已关闭但仍不流畅：瓶颈在解码或渲染', 5)
        end
        return
    end

    if bad then
        st.bad_time = st.bad_time + dt
        if st.bad_time >= o.enter_hold then degrade('lag') end
    else
        st.bad_time = 0
    end
end

-- ##########
-- 事件处理 --
-- ##########

mp.register_event('file-loaded', function()
    local was_off = st.degraded or st.probing
    st.last = nil
    st.played = 0
    st.bad_time = 0
    st.backoff = 0
    st.warned_source = false
    st.degraded = false
    st.probing = false
    st.grace_until = nil
    -- 换片会重建滤镜链，但 vf 里的 enable 标志是持久的，要显式恢复
    if was_off then
        mp.add_timeout(0.3, function() set_interp(true) end)
    end
end)

mp.register_event('end-file', function()
    -- 不清 degraded：滤镜的 disable 标志是持久的，留给下一次 file-loaded 恢复
    st.last = nil
    st.played = 0
    st.bad_time = 0
    st.backoff = 0
    st.probing = false
    st.grace_until = nil
end)

-- ##########
-- 对外接口 --
-- ##########

mp.register_script_message('on', function()
    o.enabled = true
    set_interp(true)
    st.degraded, st.probing, st.bad_time, st.backoff = false, false, 0, 0
    reset_sampling()
    note('插帧守护已开启', 2)
end)

mp.register_script_message('off', function()
    o.enabled = false
    set_interp(true)
    st.degraded, st.probing, st.bad_time = false, false, 0
    reset_sampling()
    note('插帧守护已关闭（插帧保持启用）', 2)
end)

mp.register_script_message('now', function() degrade('manual', true) end)

mp.register_script_message('status', function()
    local f = filter_entry()
    local state
    if not f then
        state = '未挂载'
    elseif is_on(f) then
        state = '启用'
    else
        state = '已禁用'
    end
    note(('补帧 %s · 速度 %.0f%% · 丢帧 %.1f/s · 退避 %ds'):format(
        state, (st.last_ratio or 1) * 100, st.last_drop_rate or 0, st.backoff), 4)
end)

mp.add_key_binding(nil, 'status', function()
    mp.command('script-message-to k7f_memc_guard status')
end)

mp.add_key_binding(nil, 'toggle', function()
    if o.enabled and not st.degraded then
        mp.command('script-message-to k7f_memc_guard off')
    else
        mp.command('script-message-to k7f_memc_guard on')
    end
end)

mp.add_periodic_timer(o.interval, sample)
msg.verbose('k7f_memc_guard 已加载')
