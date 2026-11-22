--[[
k7f_bridge.lua —— vpy ⇄ mpv 参数桥的 mpv 侧公用库

本文件只定义函数并 return，不做任何注册；由 k7f_vpypar.lua 等前端按需 require。

两条通道：
- 下行（mpv → vpy）：把参数字典拼成 `user-data="k=v;k=v"`，随 vf 重建注入。
  值里不能有空白和 `: , ; = "`，否则 vf 参数会被截断；打包脚本会跳过非法值。
- 上行（vpy → mpv）：vpy 把实际生效的参数写到 `<脚本>.k7f.json`，这里轮询文件、
  解析后用 `format_status()` 拼成一行 OSD 文本。

约定：报告文件与 vpy 同目录同名，例如：
    ~~/vs/MEMC_DLSSG_NV.vpy  →  ~~/vs/MEMC_DLSSG_NV.vpy.k7f.json
]]

local mp = require 'mp'
local utils = require 'mp.utils'

local M = {}

M.SIDECAR_SUFFIX = '.k7f.json'

local function warn(text)
	mp.msg.warn('[k7f_bridge] ' .. tostring(text))
end

-- 展开 ~~/ 与 %APPDATA% 等；失败时原样返回，让调用方按相对路径继续
function M.expand(path)
	if not path or path == '' then
		return nil
	end

	local ok, out = pcall(mp.command_native, { 'expand-path', path })
	if ok and type(out) == 'string' and out ~= '' then
		return out
	end

	return path
end

-- 与 k7f_bridge.py 的 parse_user_data 保持同一语义
function M.parse_user_data(text)
	local opts = {}
	if not text then
		return opts
	end

	local normalized = tostring(text):gsub(',', ';')
	for part in normalized:gmatch('[^;]+') do
		local key, val = part:match('^%s*([^=]-)%s*=%s*(.-)%s*$')
		if key and key ~= '' then
			opts[key:lower()] = val
		end
	end

	return opts
end

-- vf 参数用 `:` 分隔，所以值里出现这些字符会破坏整条滤镜
function M.encode_value(value)
	local text = tostring(value)

	if text == '' then
		return false, '空值'
	end

	if text:find('[%s:,;="]') then
		return false, '值里不能有空白或 : , ; = "'
	end

	return true, text
end

-- 按 order 的顺序拼 user-data；order 省略时按键名排序，保证结果稳定
function M.build_user_data(state, order)
	if type(state) ~= 'table' then
		return ''
	end

	if not order then
		order = {}
		for key in pairs(state) do
			order[#order + 1] = key
		end
		table.sort(order)
	end

	local parts = {}
	for _, key in ipairs(order) do
		local value = state[key]
		if value ~= nil then
			local ok, encoded = M.encode_value(value)
			if ok then
				parts[#parts + 1] = key .. '=' .. encoded
			else
				warn(('跳过参数 %s=%s：%s'):format(tostring(key), tostring(value), encoded))
			end
		end
	end

	return table.concat(parts, ';')
end

-- 由脚本路径得到报告文件的路径
function M.status_path(script)
	local path = M.expand(script)
	if not path then
		return nil
	end

	return path .. M.SIDECAR_SUFFIX
end

function M.read_text(path)
	if not path then
		return nil, '路径为空'
	end

	local fh = io.open(path, 'r')
	if not fh then
		return nil, '打不开文件'
	end

	local text = fh:read('*a')
	fh:close()

	return text
end

function M.read_json(path)
	local text, err = M.read_text(path)
	if not text or text == '' then
		return nil, err or '文件为空'
	end

	local ok, data = pcall(utils.parse_json, text)
	if not ok or type(data) ~= 'table' then
		return nil, '解析失败'
	end

	return data
end

-- script_or_path 可以是 vpy 路径，也可以是报告文件路径
function M.read_status(script_or_path)
	if not script_or_path then
		return nil, nil, '路径为空'
	end

	local path = script_or_path
	if path:sub(-#M.SIDECAR_SUFFIX) ~= M.SIDECAR_SUFFIX then
		path = M.status_path(script_or_path)
	end

	local data, err = M.read_json(path)
	return data, path, err
end

function M.write_json(path, data)
	if not path then
		return false, '路径为空'
	end

	local ok, text = pcall(utils.format_json, data, { indent = '    ' })
	if not ok or not text then
		return false, 'utils.format_json 不可用'
	end

	local fh, err = io.open(path, 'w')
	if not fh then
		local dir = utils.split_path(path)
		if dir and dir ~= '' and utils.mkdir_p then
			pcall(utils.mkdir_p, dir)
			fh, err = io.open(path, 'w')
		end
	end

	if not fh then
		return false, tostring(err)
	end

	local written, werr = fh:write(text)
	fh:close()

	return written and true or false, werr
end

-- 轮询报告文件：内容变了才回调；返回 timer，可用 :kill() 停掉
function M.watch_status(script, interval, callback)
	local path = M.status_path(script)
	if not path then
		return nil
	end

	local last_text = nil
	local timer = mp.add_periodic_timer(interval or 0.5, function()
		local fh = io.open(path, 'r')
		if not fh then
			-- 文件消失（换了脚本/清缓存）：通知一次，避免一直显示旧报告
			if last_text ~= nil then
				last_text = nil
				callback(nil, path)
			end
			return
		end

		local text = fh:read('*a')
		fh:close()

		if text == last_text then
			return
		end
		last_text = text

		local ok, data = pcall(utils.parse_json, text)
		if ok and type(data) == 'table' then
			callback(data, path)
		end
	end)

	return timer
end

-- 把报告拼成一行 OSD：标题 ｜ 摘要 ｜ 1080p→1440p ｜ 24→72fps ｜ 提示 ｜ 实测
-- hide_title 用于已经带前缀的场合（例如 “实际：…”），避免重复显示脚本名
function M.format_status(data, fallback_title, hide_title)
	if type(data) ~= 'table' then
		return nil
	end

	local parts = {}

	if not hide_title then
		local title = data.title or fallback_title
		if title and title ~= '' then
			parts[#parts + 1] = title
		end
	end

	if data.ok == false then
		parts[#parts + 1] = '⚠ 初始化失败'
	end

	if data.summary and data.summary ~= '' then
		parts[#parts + 1] = tostring(data.summary)
	end

	local in_height = data.input and tonumber(data.input.h)
	local out_height = data.output and tonumber(data.output.h)
	if out_height then
		if in_height and in_height ~= out_height then
			parts[#parts + 1] = ('%dp→%dp'):format(in_height, out_height)
		else
			parts[#parts + 1] = ('%dp'):format(out_height)
		end
	end

	local in_fps = data.input and tonumber(data.input.fps)
	local out_fps = data.output and tonumber(data.output.fps)
	if out_fps and out_fps > 0 then
		if in_fps and in_fps > 0 and math.abs(out_fps - in_fps) > 0.05 then
			parts[#parts + 1] = ('%.0f→%.0ffps'):format(in_fps, out_fps)
		else
			parts[#parts + 1] = ('%.2ffps'):format(out_fps)
		end
	end

	if data.note and data.note ~= '' then
		parts[#parts + 1] = tostring(data.note)
	end

	-- 播放中才有意义；暂停/未出帧时返回 nil
	local measured = mp.get_property_number('estimated-vf-fps')
	if measured and measured > 0 then
		parts[#parts + 1] = ('实测 %.1ffps'):format(measured)
	end

	return table.concat(parts, ' ｜ ')
end

return M
