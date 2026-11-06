-- Xerces: voice chat starts as push-to-talk on a new game profile (docs/DESIGN.md 10.5).
--
-- The game keeps the choice in the player's profile: the "Push to talk" switch in its voice options is the
-- profile setting snd_voicechat_open_mic, 1 for an open mic and 0 for push-to-talk (lua_fixes/__init__.lua),
-- and the talk key is the +talk bind, B by default. The client decides as it starts
-- (src/client/xerces/voice_default.cpp, src/xerces_core/voice_default.hpp): Dvar.xerces_voice_ptt is 1 only
-- when the game had no profile yet, so there is no choice of the player's to keep. A profile that was there
-- keeps its setting, and nothing here changes it.
if not CoD.isFrontend or not Dvar.xerces_voice_ptt or Dvar.xerces_voice_ptt:get() ~= 1 then
  return
end

-- One attempt: marked as tried (3) before the profile is touched, so a later front end this session never
-- sets it again, even when this attempt fails and the player has since chosen in the options.
Engine.SetDvar("xerces_voice_ptt", 3)

local function set_push_to_talk()
  local controller = Engine.GetPrimaryController() or 0
  if Engine.GetProfileVarInt(controller, "snd_voicechat_open_mic") ~= 0 then
    Engine.SetProfileVar(controller, "snd_voicechat_open_mic", 0)
    Engine.CommitProfileChanges(controller)
  end
  return Engine.GetProfileVarInt(controller, "snd_voicechat_open_mic") == 0
end

local ok, done = pcall(set_push_to_talk)
if ok and done then
  Engine.SetDvar("xerces_voice_ptt", 2)
end
