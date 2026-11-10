-- lua/modules/godmode.lua
Cheat.register('godmode', {
    tick = function()
        local ped = PlayerPedId()
        SetPlayerInvincible(PlayerId(), true)
        SetEntityInvincible(ped, true)
        SetPedCanRagdoll(ped, false)
        SetEntityProofs(ped, true, true, true, true, true, true, true, true)
    end,
    stop = function()
        SetPlayerInvincible(PlayerId(), false)
        SetEntityInvincible(PlayerPedId(), false)
        SetPedCanRagdoll(PlayerPedId(), true)
    end
})