-- lua/modules/aimbot.lua
local function closest(fov)
    local cam = GetGameplayCamCoord()
    local best, bd = nil, fov
    for _, p in ipairs(GetActivePlayers()) do
        if p ~= PlayerId() then
            local ped = GetPlayerPed(p)
            if not IsEntityDead(ped) and not IsPedDeadOrDying(ped, true) then
                local bone = GetPedBoneIndex(ped, 0x796E)
                local bx, by, bz = table.unpack(GetWorldPositionOfEntityBone(ped, bone))
                local d = #(vector3(bx, by, bz) - cam)
                local _, sx, sy = GetScreenCoordFromWorldCoord(bx, by, bz)
                if sx then
                    local c = math.sqrt((sx - 0.5) ^ 2 + (sy - 0.5) ^ 2) * 100
                    if c < bd then bd, best = c, { x = bx, y = by, z = bz } end
                end
            end
        end
    end
    return best
end

Cheat.register('aimbot', {
    tick = function()
        if not IsControlPressed(0, 25) then return end
        local t = closest(Cheat.get('aim_fov') or 90)
        if t then SetGameplayCamRelativeHeading(0) end
        if t then
            local _, sx, sy = GetScreenCoordFromWorldCoord(t.x, t.y, t.z)
            if sx then
                local cx, cy = GetControlNormal(0, 220), GetControlNormal(0, 221)
                SetControlNormal(0, 220, cx + (sx - 0.5) * 0.06)
                SetControlNormal(0, 221, cy + (sy - 0.5) * 0.06)
            end
        end
    end
})