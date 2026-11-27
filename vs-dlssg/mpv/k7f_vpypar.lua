--[[
k7f_vpypar.lua —— 播放中用 OSD 菜单改 VapourSynth 补帧脚本的参数

原理：vpy 脚本本身读不到 mpv 的属性，但 `vf=vapoursynth` 有 user-data 子参数，
mpv 会把它作为全局变量 user_data 注入脚本（默认空串；mpv 每次 seek 都会重跑脚本，
所以跳转/重建后参数一定生效）。本脚本维护一份参数状态，改动后重建滤镜：

    vf append @<label>:vapoursynth="<脚本>":user-data="multi=4;scene=0.35"

上行：vpy 把实际生效的参数写到脚本同目录的 `<脚本>.k7f.json`，这里用
`k7f_bridge.watch_status()` 轮询（默认 0.5s），OSD 可显示“设置”和“实际”两行。
下行字符串与上行文件格式都由 k7f_bridge.lua / k7f_bridge.py 统一定义。

注意：改参 = 整条滤镜重建（引擎要重新初始化），不是逐帧实时；
multi 会改变输出帧数，本来也只能重建。

部署：
  1) k7f_bridge.lua 与本文件一起放到 portable_config\scripts\
  2) input.conf（或 input_uosc.conf）加一行，键自选：
       V  script-binding k7f_vpypar/menu
  3) 用 OSD 菜单改参；只有同名标签的 vapoursynth 滤镜在跑时才真正重建，否则只存状态。

可选 script-opts（script-opts.conf）：
  script-opts-append = k7f_vpypar-label=VDLSSG
  script-opts-append = k7f_vpypar-script=~~/vs/MEMC_DLSSG_NV.vpy

脚本消息（给别的脚本/按键用）：
  script-message-to k7f_vpypar set multi 4
  script-message-to k7f_vpypar apply
  script-message-to k7f_vpypar status
]]

local mp = require 'mp'
local msg = require 'mp.msg'
local input = require 'mp.input'

-- mpv 在生产环境会把 ~~/scripts 加进 package.path；开发时脚本可能在任意目录，
-- 所以 require 失败后先找脚本同目录，再退回 ~~/scripts/
local function load_bridge()
    local ok, mod = pcall(require, 'k7f_bridge')
    if ok and type(mod) == 'table' then
        return mod
    end

    local source = debug.getinfo(1, 'S').source or ''
    local here = source:match('^@(.*[/\\])') or ''
    local candidates = {
        here .. 'k7f_bridge.lua',
        mp.command_native({ 'expand-path', '~~/scripts/k7f_bridge.lua' }),
    }
    for _, path in ipairs(candidates) do
        local chunk = path and loadfile(path)
        if chunk then
            local loaded = chunk()
            if type(loaded) == 'table' then return loaded end
        end
    end

    error('k7f_vpypar: 找不到 k7f_bridge.lua（与 k7f_vpypar.lua 放同一目录）')
end

local B = load_bridge()

local o = {
    -- 滤镜标签：要和 vf 里的 @标签 一致（menu.conf 的 `vf set vapoursynth=…` 没有标签，
    -- 这时本脚本不会去改它，见 README 里的“加标签”写法）
    label = 'VDLSSG',
    -- 对应的 vpy 脚本（写 vf 时原样带上）
    script = '~~/vs/MEMC_DLSSG_NV.vpy',
    -- 参数状态落盘位置，换片/重启 mpv 后沿用上次的选择
    state_file = '~~/_cache/k7f_vpypar.json',
    -- 上行报告轮询间隔（秒）；0.5s 够快，又不会明显增加 IO
    status_interval = 0.5,
}
require 'mp.options'.read_options(o, 'k7f_vpypar')

-- 参数表：key 必须和 vpy 里 _parse_user_data() 认的键一致
local params = {
    {
        key = 'multi', name = '倍数', default = '3',
        choices = { { '2×', '2' }, { '3×', '3' }, { '4×', '4' }, { '5×', '5' }, { '6×', '6' } },
    },
    {
        key = 'preset', name = '模型预设(仅DLSSG)', default = 'a',
        choices = { { 'A · 默认模型', 'a' }, { 'C · FG Lite（生成帧快 ~13%）', 'c' } },
    },
    {
        key = 'scene', name = '切镜阈值', default = '0.12',
        choices = { { '关闭', '0' }, { '0.08', '0.08' }, { '0.12', '0.12' }, { '0.15', '0.15' }, { '0.20', '0.20' } },
    },
    {
        key = 'duplicate', name = '重复帧阈值', default = '0.005',
        choices = { { '关闭', '0' }, { '0.003', '0.003' }, { '0.005', '0.005' }, { '0.01', '0.01' } },
    },
    {
        key = 'pre_h', name = '预缩高度', default = '0',
        choices = { { '关', '0' }, { '720', '720' }, { '1080', '1080' }, { '1440', '1440' } },
    },
    {
        key = 'log', name = '耗时日志', default = '0',
        choices = { { '关', '0' }, { '开', '1' } },
    },
}

-- 模型档位只有 VFG 有（k7sfunc.mod_vpy 只给 vvfg 认 mode）；别的引擎放进去只会
-- 让 OSD 多一行没作用的项，所以按脚本名决定加不加
local VFG_SCRIPTS = { ['memc_vfg_nv.vpy'] = true }

local function script_basename(path)
    local name = tostring(path or ''):gsub('\\', '/')
    return (name:match('[^/]+$') or ''):lower()
end

if VFG_SCRIPTS[script_basename(o.script)] then
    -- 插在倍数后面：决定输出帧数的先选，再选画质/速度
    table.insert(params, 2, {
        key = 'mode', name = '档位', default = '1',
        choices = { { '低', '0' }, { '中', '1' }, { '高', '2' } },
    })
end

local ORDER = {}
for i, p in ipairs(params) do
    ORDER[i] = p.key
end

local state = {}

-- 最近一次 vpy 报告（上行）；nil 表示还没收到
local remote = nil
-- 最近一次重建的时间戳：只有重建后短时间内的报告才主动弹 OSD
local applied_at = 0
local APPLY_REPORT_WINDOW = 5

-- ##########
-- 状态读写 --
-- ##########

local function state_path()
    return B.expand(o.state_file)
end

local function valid_value(p, val)
    for _, c in ipairs(p.choices) do
        if c[2] == val then return true end
    end
    return false
end

local function find_param(key)
    for _, p in ipairs(params) do
        if p.key == key then return p end
    end
    return nil
end

local function load_state()
    local saved = B.read_json(state_path())

    -- 兼容旧版逐行 k=v 的状态文件（那时没有 utils.format_json）
    local text
    if not saved then
        text = B.read_text(state_path())
        if text and text ~= '' then
            saved = {}
            for k, v in text:gmatch('([%w_]+)%s*=%s*([^%s]+)') do
                saved[k] = v
            end
        end
    end
    -- 文件在、两种格式都读不出来：别静默退回默认值，不然“改过的参数又变回去了”查不到原因
    if not saved and text and text ~= '' then
        msg.warn('参数状态文件读不出来，已按默认值处理：' .. tostring(state_path()))
    end
    saved = saved or {}

    for _, p in ipairs(params) do
        local val = saved[p.key]
        -- 候选值全是字符串：JSON 里的数字/布尔先归一化，再校验
        if type(val) == 'number' then
            val = tostring(val)
        elseif type(val) == 'boolean' then
            val = val and '1' or '0'
        end
        state[p.key] = valid_value(p, val) and val or p.default
    end
end

local function save_state()
    local ok, err = B.write_json(state_path(), state)
    if not ok then
        msg.warn('状态写入失败：' .. tostring(state_path()) .. '（' .. tostring(err) .. '，本次仅内存生效）')
    end
end

local function value_label(p)
    for _, c in ipairs(p.choices) do
        if c[2] == state[p.key] then return c[1] end
    end
    return state[p.key] or '?'
end

local function describe()
    local out = {}
    for _, p in ipairs(params) do
        out[#out + 1] = p.name .. ' ' .. value_label(p)
    end
    return table.concat(out, ' · ')
end

-- ############################
-- 组装 user-data 并重建滤镜 --
-- ############################

local function build_user_data()
    return B.build_user_data(state, ORDER)
end

-- 两个 vpy 路径是不是同一个脚本：只比文件名（链里存的是绝对路径，配置里可能是 ~~/）
local function file_matches(a, b)
    if not a or not b then return false end
    local function norm(p)
        return (p:gsub('\\', '/'):match('[^/]+$') or p):lower()
    end
    return norm(a) == norm(b)
end

-- 收集**所有**指向本 vpy 的条目：标签相同的，或文件名对得上的无标签 vapoursynth。
-- 正常情况下只有一条；多于一条说明之前有次移除没匹配上（见 apply 里的说明），
-- 那会让补帧滤镜串联（两条 ×5 = 25 倍，OSD 上就是 600fps），必须合并掉。
local function find_entries()
    local list = mp.get_property_native('vf') or {}
    local out = {}
    for i, f in ipairs(list) do
        local ours = (f.label == o.label)
            or (f.name == 'vapoursynth' and f.params and file_matches(f.params.file, o.script))
        if ours then
            out[#out + 1] = { index = i, filter = f }
        end
    end
    return list, out
end

-- user-data 是 "k=v;k=v" 串：键序不同、内容相同的两份设置视为相同。
-- k7f_multi 会按自己的键序改写这条串，若这里只比字符串就会把“同样设置”当成变化，
-- 无谓地重建滤镜（每次都重初始化引擎）；两边都在写时还会来回打架。
local function same_user_data(a, b)
    if a == b then return true end
    local pa = B.parse_user_data(a or '')
    local pb = B.parse_user_data(b or '')
    local na, nb = 0, 0
    for _ in pairs(pa) do na = na + 1 end
    for _ in pairs(pb) do nb = nb + 1 end
    if na ~= nb then return false end
    for k, v in pairs(pa) do
        if tostring(pb[k]) ~= tostring(v) then return false end
    end
    return true
end

-- 正在重建时忽略 vf 属性回调，避免自己触发自己
local applying = false

-- ~~/ 在 --no-config 或未设配置目录时会展开成相对路径，这时保留原串交给 mpv
local function resolve_path(p)
    local ex = mp.command_native({ 'expand-path', p })
    if ex and (ex:match('^%a:[/\\]') or ex:sub(1, 1) == '/' or ex:match('^\\\\')) then
        return ex
    end
    return p
end

local function apply(silent)
    save_state()

    -- 设置变了，旧报告不再对应当前选择；等 vpy 重建后写新报告
    remote = nil

    local list, entries = find_entries()
    local f = entries[1] and entries[1].filter
    if not f or f.name ~= 'vapoursynth' then
        if not silent then
            mp.osd_message(('参数已保存（滤镜 %s 未启用）：%s'):format(o.label, describe()), 3)
        end
        return false
    end

    -- script 可能写成 ~~/vs/…：能解析成绝对路径就用绝对路径
    local script = resolve_path(o.script)
    local ud = build_user_data()

    -- 整链重建：剔掉所有指向本 vpy 的旧条目，末尾放一条带标签的新条目。
    -- 不用 `vf remove <spec>`：它按 name+全部参数做内容匹配，条目只要被别的脚本
    -- 加过 user-data 就匹配不上，而 mpv 对“没找到”是静默成功——旧条目留在链上，
    -- 新条目 append 上去就变成两条补帧串联（倍数相乘，×5 变 25 倍 / 600fps）。
    local chain = {}
    for _, filter in ipairs(list) do
        local ours = (filter.label == o.label)
            or (filter.name == 'vapoursynth' and filter.params
                and file_matches(filter.params.file, o.script))
        if not ours then
            chain[#chain + 1] = filter
        end
    end
    local dropped = #list - #chain
    chain[#chain + 1] = {
        name = 'vapoursynth',
        label = o.label,
        enabled = true,
        params = { file = script, ['user-data'] = ud },
    }

    applying = true
    local ok, err = pcall(mp.set_property_native, 'vf', chain)
    applying = false
    if not ok then
        local text = '重建滤镜失败：' .. tostring(err)
        msg.error(text)
        mp.osd_message('重建滤镜失败，见控制台日志', 3)
        return false
    end

    -- 兜底复核：mpv 里“命令没生效”常常也是成功返回，改完链后自己数一遍
    local _, after = find_entries()
    if #after ~= 1 then
        local text = ('滤镜链里仍有 %d 条 %s（应为 1 条）'):format(#after, o.label)
        msg.error(text)
        mp.osd_message(text, 3)
        return false
    end
    if dropped > 1 then
        local text = ('清理了 %d 条重复的 %s 滤镜（多条补帧会叠乘倍数）'):format(dropped - 1, o.label)
        msg.warn(text)
        mp.osd_message(text, 3)
    end

    applied_at = os.time()
    if not silent then
        mp.osd_message(('已应用：%s'):format(describe()), 2)
    end
    return true
end

-- ###############
-- 上行报告监看 --
-- ###############

local watcher = B.watch_status(o.script, o.status_interval, function(data)
    remote = data

    -- 只在刚重建后主动回显一次“实际参数”；平时不打扰，避免 seek 后反复弹
    if data and os.time() - applied_at <= APPLY_REPORT_WINDOW then
        local line = B.format_status(data, nil, true)
        if line then
            mp.osd_message('实际：' .. line, 2)
        end
    end
end)

local function status_text()
    local lines = { ('%s 设置：%s'):format(o.label, describe()) }
    if remote then
        local actual = B.format_status(remote, nil, true)
        if actual then
            lines[#lines + 1] = '实际：' .. actual
        end
    end
    return table.concat(lines, '\n')
end

-- ###############
-- OSD 菜单本体 --
-- ###############

local open_param_menu  -- 前向声明：选择完数值后要回到这里

local function open_value_menu(p)
    local items, values, default_item = {}, {}, nil
    for i, c in ipairs(p.choices) do
        items[i] = c[1] .. (c[2] == state[p.key] and '  ●' or '')
        values[i] = c[2]
        if c[2] == state[p.key] then default_item = i end
    end

    input.select {
        prompt = ('%s   （←/→ 或回车确认 · Esc 返回）'):format(p.name),
        items = items,
        default_item = default_item,
        submit = function(index)
            state[p.key] = values[index]
            apply(false)
            mp.add_timeout(0.05, open_param_menu)
        end,
    }
end

open_param_menu = function()
    local items = {}
    for i, p in ipairs(params) do
        items[i] = ('%s：%s'):format(p.name, value_label(p))
    end

    input.select {
        prompt = ('%s 补帧参数   （回车修改 · Esc 关闭）'):format(o.label),
        items = items,
        submit = function(index)
            open_value_menu(params[index])
        end,
    }
end

-- ##########
-- 对外接口 --
-- ##########

mp.add_key_binding(nil, 'menu', open_param_menu)

mp.register_script_message('set', function(key, value)
    for _, p in ipairs(params) do
        if p.key == key then
            if not valid_value(p, value) then
                msg.warn(('不认识的取值：%s=%s'):format(key, tostring(value)))
                return
            end
            state[p.key] = value
            apply(false)
            return
        end
    end
    msg.warn('不认识的参数：' .. tostring(key))
end)

mp.register_script_message('apply', function() apply(false) end)

mp.register_script_message('menu', open_param_menu)
mp.register_script_message('close', function() input.terminate() end)

mp.register_script_message('status', function()
    local text = status_text()
    -- 同时写日志：OSD 不落盘，排错和端到端测试都靠这一行
    mp.msg.info(text)
    mp.osd_message(text, 4)
end)

-- 用户用别的方式（menu.conf 预设等）挂上同名滤镜时，把落盘的参数补回去，
-- 免得点了预设又回到脚本里的硬编码默认值
mp.observe_property('vf', 'none', function()
    if applying then return end
    local _, entries = find_entries()
    if #entries == 0 then
        remote = nil
        return
    end
    -- 多于一条：有旧条目没被清掉（见 apply 的说明），合并成一条
    if #entries > 1 then
        msg.warn(('%s 在滤镜链里有 %d 条，合并为一条'):format(o.label, #entries))
        mp.add_timeout(0.05, function() apply(true) end)
        return
    end
    local f = entries[1].filter
    if f.name ~= 'vapoursynth' then
        remote = nil
        return
    end
    local ud = f.params and f.params['user-data'] or ''

    -- 倍数归 k7f_multi 统一管（它的菜单会往 user-data 里写 multi）。链上已经有值
    -- 时以链为准，否则两边各写各的会陷入“写 3 → 写 5 → 写 3”的无限重建。
    local p_multi = find_param('multi')
    local on_chain = B.parse_user_data(ud).multi
    if p_multi and on_chain ~= nil then
        on_chain = tostring(on_chain)
        if valid_value(p_multi, on_chain) and state.multi ~= on_chain then
            state.multi = on_chain
            save_state()
            msg.verbose('倍数以滤镜链为准：' .. on_chain)
        end
    end

    if same_user_data(ud, build_user_data()) then return end
    mp.add_timeout(0.05, function() apply(true) end)
end)

load_state()
msg.verbose(('已加载 %s，当前：%s（报告：%s）')
    :format(o.label, describe(), tostring(watcher and '监看中' or '未启用')))
