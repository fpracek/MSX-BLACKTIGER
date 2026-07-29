local fields = {}
local frame = 0
local function setup()
    local ports = manager.machine.ioport.ports
    fields.coin  = ports[":IN0"].fields["Coin 1"]
    fields.start = ports[":IN0"].fields["1 Player Start"]
    fields.jump  = ports[":IN1"].fields["P1 Button 2"]
end
local function tick()
    frame = frame + 1
    if frame == 2 then setup() end
    if frame < 2 then return end
    fields.coin:set_value(frame >= 60 and frame < 64 and 1 or 0)
    fields.start:set_value(frame >= 180 and frame < 184 and 1 or 0)
    if frame > 700 then
        local ph = frame % 90
        fields.jump:set_value(ph < 8 and 1 or 0)
        if ph == 16 then manager.machine.video:snapshot() end
    end
    if frame > 3400 then manager.machine:exit() end
end
GLOBAL_TICK = emu.add_machine_frame_notifier(tick)
