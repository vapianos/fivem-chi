-- lua/modules/esp.lua
local function w2s(x, y, z)
    local on, sx, sy = GetScreenCoordFromWorldCoord(x, y, z)
    return on and sx or nil, sy
end

Cheat.register('esp', {
    tick = function()
        local me = PlayerId()
        for _, p in ipairs(GetActivePlayers()) do
            if p ~= me then
                local ped = GetPlayerPed(p)
                if ped ~= 0 and not IsEntityDead(ped) then
                    local x, y, z = table.unpack(GetEntityCoords(ped))
                    local sx, sy = w2s(x, y, z + 1.05)
                    if sx then
                        local hp = GetEntityHealth(ped) - 100
                        DrawRect(sx, sy - 0.02, 0.0006, 0.045, 0, 220, 120, 200)
                        DrawRect(sx, sy + 0.02 + (0.045 * (1 - hp / 100)) / 2, 0.0006,
                                 0.045 * (hp / 100), 80, 255, 120, 220)
                        if Cheat.get('esp_names') then
                            SetTextFont(4); SetTextScale(0.0, 0.32)
                            SetTextColour(255, 255, 255, 220)
                            SetTextCentre(true)
                            BeginTextCommandDisplayText('STRING')
                            AddTextComponentSubstringPlayerName(('%s [%dm]'):format(
                                GetPlayerName(p), math.floor(#(GetEntityCoords(ped) - GetEntityCoords(PlayerPedId())))))
                            EndTextCommandDisplayText(sx, sy + 0.03)
                        end
                    end
                end
            end
        end
    end
})