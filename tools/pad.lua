-- PCSX-Redux test helper: press / release pad-1 buttons from outside the emulator, through
-- the real controller path (SIO0 -> BIOS pad driver -> psyqo::SimplePad -> ReadInputDevice).
--
-- Load at boot:   PCSX-Redux ... -webserver -exec "dofile('/abs/path/tools/pad.lua')"
-- Then:           curl 'http://localhost:8080/api/v1/lua/pad?press=Start'
--                 curl 'http://localhost:8080/api/v1/lua/pad?release=Start'
--                 curl 'http://localhost:8080/api/v1/lua/pad?press=Right,Cross&release=Start'
--                 curl 'http://localhost:8080/api/v1/lua/pad?clear=all'
-- Works while GDB holds the CPU at a breakpoint (the web server keeps running), so GDB
-- scripts can press buttons at exact game frames with `shell curl ...`.
-- Button bits = the PS1 pad word, same order as psyqo::SimplePad::Button.

local bits = {
    Select = 0, L3 = 1, R3 = 2, Start = 3, Up = 4, Right = 5, Down = 6, Left = 7,
    L2 = 8, R2 = 9, L1 = 10, R1 = 11, Triangle = 12, Circle = 13, Cross = 14, Square = 15,
}

PCSX.WebServer = PCSX.WebServer or {}
PCSX.WebServer.Handlers = PCSX.WebServer.Handlers or {}

PCSX.WebServer.Handlers.pad = function(request)
    local pad = PCSX.SIO0.slots[1].pads[1]
    local query = request.urlData.query or ''
    local done = {}
    for key, value in string.gmatch(query, '([^&=]+)=([^&]*)') do
        for name in string.gmatch(value, '[^,]+') do
            if key == 'clear' and name == 'all' then
                for _, b in pairs(bits) do pad.clearOverride(b) end
                done[#done + 1] = 'clear:all'
            elseif bits[name] ~= nil then
                if key == 'press' then
                    pad.setOverride(bits[name])
                elseif key == 'release' then
                    pad.clearOverride(bits[name])
                end
                done[#done + 1] = key .. ':' .. name
            else
                done[#done + 1] = 'unknown:' .. name
            end
        end
    end
    return 'pad ' .. table.concat(done, ' ') .. '\n'
end

print('[pad.lua] /api/v1/lua/pad ready')
