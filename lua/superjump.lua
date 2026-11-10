-- lua/modules/superjump.lua
Cheat.register('superjump', {
    tick = function()
        if IsControlJustPressed(0, 22) then
            SetSuperJumpThisFrame(PlayerId())
            local ped = PlayerPedId()
            SetEntityVelocity(ped, GetEntityVelocity(ped).x, GetEntityVelocity(ped).y, 8.0)
        end
    end
})