-- ripsprites.lua - Black Tiger sprite RAM logger for MAME (autoboot_script)
-- Auto-plays like riptiger.lua and appends 0xF800-0xFFFF (2KB, includes the
-- sprite table) each frame during gameplay to one binary log, plus scroll csv.

local OUT = "E:/TEMP/Claude/BlackTiger-MSX/rip/"
local FRAME_FIRST = 700
local FRAME_LAST = 4000

local prog, iosp
local scrollx, scrolly = 0, 0
taps2 = {}                 -- GLOBAL: keep tap objects alive
local frame = 0
local binf, csvf
local fields = {}

local function setup()
    local machine = manager.machine
    local cpu = machine.devices[":maincpu"]
    prog = cpu.spaces["program"]
    iosp = cpu.spaces["io"]
    binf = io.open(OUT .. "sprites_log.bin", "wb")
    csvf = io.open(OUT .. "sprites_scroll.csv", "w")

    taps2[#taps2 + 1] = iosp:install_write_tap(0x00, 0xff, "ports2",
        function(offset, data, mask)
            local p = offset & 0xff
            local d = data & 0xff
            if p == 0x08 then scrollx = (scrollx & 0xff00) | d
            elseif p == 0x09 then scrollx = (scrollx & 0x00ff) | (d << 8)
            elseif p == 0x0a then scrolly = (scrolly & 0xff00) | d
            elseif p == 0x0b then scrolly = (scrolly & 0x00ff) | (d << 8)
            end
        end)

    local ports = manager.machine.ioport.ports
    fields.coin  = ports[":IN0"].fields["Coin 1"]
    fields.start = ports[":IN0"].fields["1 Player Start"]
    fields.right = ports[":IN1"].fields["P1 Right"]
    fields.atk   = ports[":IN1"].fields["P1 Button 1"]
    fields.jump  = ports[":IN1"].fields["P1 Button 2"]
end

local function drive_inputs()
    fields.coin:set_value((frame % 1800) < 4 and 1 or 0)
    fields.start:set_value((frame % 600) >= 30 and (frame % 600) < 34 and 1 or 0)
    fields.right:set_value(frame > 400 and 1 or 0)
    fields.atk:set_value((frame % 8) < 4 and 1 or 0)
    fields.jump:set_value((frame % 100) < 10 and 1 or 0)
end

local function on_frame()
    frame = frame + 1
    drive_inputs()
    if frame >= FRAME_FIRST and frame <= FRAME_LAST then
        local t = {}
        for a = 0xf800, 0xffff do
            t[#t + 1] = string.char(prog:read_u8(a))
        end
        binf:write(table.concat(t))
        csvf:write(string.format("%d,%d,%d\n", frame, scrollx, scrolly))
    end
    if frame == FRAME_LAST then
        binf:close()
        csvf:close()
    end
end

setup()
if emu.register_frame_done then
    emu.register_frame_done(on_frame)
else
    frame_sub2 = emu.add_machine_frame_notifier(on_frame)
end
