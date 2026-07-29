-- riptiger.lua - Black Tiger asset ripper for MAME (autoboot_script)
-- Shadows bg tilemap RAM writes (0xC000-0xCFFF banked window), tracks scroll
-- and videoram bank ports, saves unique palette snapshots (0xD800-0xDFFF).

local OUT = "E:/TEMP/Claude/BlackTiger-MSX/rip/"

local prog, iosp
local bank = 0
local scrollx, scrolly = 0, 0
local layout = 0
taps = {}                  -- GLOBAL: keeps tap objects alive (GC removes taps!)
local shadow = {}          -- shadow[bank] = table offset->byte (0..0xfff)
local pal_seen = {}
local pal_count = 0
local frame = 0
local logf

local function log(msg)
    if logf then logf:write(msg .. "\n"); logf:flush() end
end

local function setup()
    local machine = manager.machine
    local cpu = machine.devices[":maincpu"]
    prog = cpu.spaces["program"]
    iosp = cpu.spaces["io"]
    for b = 0, 3 do
        local t = {}
        for i = 0, 0xfff do t[i] = 0 end
        shadow[b] = t
    end
    logf = io.open(OUT .. "rip_log.txt", "w")

    taps[#taps + 1] = prog:install_write_tap(0xc000, 0xcfff, "bgram",
        function(offset, data, mask)
            shadow[bank][offset & 0xfff] = data & 0xff
        end)

    taps[#taps + 1] = iosp:install_write_tap(0x00, 0xff, "ports",
        function(offset, data, mask)
            local p = offset & 0xff
            local d = data & 0xff
            if p == 0x08 then scrollx = (scrollx & 0xff00) | d
            elseif p == 0x09 then scrollx = (scrollx & 0x00ff) | (d << 8)
            elseif p == 0x0a then scrolly = (scrolly & 0xff00) | d
            elseif p == 0x0b then scrolly = (scrolly & 0x00ff) | (d << 8)
            elseif p == 0x0c then bank = d & 0x03
            elseif p == 0x0d then layout = d
            end
        end)

    log("setup done, taps installed")
end

local function palette_snapshot()
    local t = {}
    for a = 0xd800, 0xdfff do
        t[#t + 1] = string.char(prog:read_u8(a))
    end
    return table.concat(t)
end

local function dump_shadow(tag)
    local f = io.open(OUT .. string.format("bg_%s.bin", tag), "wb")
    if not f then return end
    for b = 0, 3 do
        local t = shadow[b]
        local chunk = {}
        for i = 0, 0xfff do chunk[i + 1] = string.char(t[i]) end
        f:write(table.concat(chunk))
    end
    f:close()
end

-- blind auto-play: coin, start, hold right, mash attack, periodic jump
local fields = {}

local function setup_inputs()
    local ports = manager.machine.ioport.ports
    fields.coin   = ports[":IN0"].fields["Coin 1"]
    fields.start  = ports[":IN0"].fields["1 Player Start"]
    fields.right  = ports[":IN1"].fields["P1 Right"]
    fields.atk    = ports[":IN1"].fields["P1 Button 1"]
    fields.jump   = ports[":IN1"].fields["P1 Button 2"]
end

local function drive_inputs()
    fields.coin:set_value((frame % 1800) < 4 and 1 or 0)
    fields.start:set_value((frame % 600) >= 30 and (frame % 600) < 34 and 1 or 0)
    fields.right:set_value(frame > 400 and 1 or 0)
    fields.atk:set_value((frame % 8) < 4 and 1 or 0)
    fields.jump:set_value((frame % 100) < 10 and 1 or 0)
end

local scroll_csv

local function on_frame()
    frame = frame + 1
    drive_inputs()
    if frame % 30 == 0 then
        scroll_csv:write(string.format("%d,%d,%d,%d\n", frame, scrollx, scrolly, bank))
    end

    -- palette: save every distinct content (cap 256)
    if frame % 10 == 0 and pal_count < 256 then
        local pal = palette_snapshot()
        if not pal_seen[pal] then
            pal_seen[pal] = true
            pal_count = pal_count + 1
            local f = io.open(OUT .. string.format("pal_%04d_f%06d.bin", pal_count, frame), "wb")
            if f then f:write(pal); f:close() end
            log(string.format("frame %d: new palette #%d", frame, pal_count))
        end
    end

    -- bg shadow snapshot every 5s of emulated time
    if frame % 300 == 0 then
        dump_shadow(string.format("f%06d", frame))
        log(string.format("frame %d: bg dump, scrollx=%d scrolly=%d bank=%d layout=0x%02x",
            frame, scrollx, scrolly, bank, layout))
    end
end

setup()
setup_inputs()
scroll_csv = io.open(OUT .. "scroll.csv", "w")
if emu.register_frame_done then
    emu.register_frame_done(on_frame)
else
    frame_sub = emu.add_machine_frame_notifier(on_frame)
end
