-- Xerces: voice chat starts as push-to-talk in a Xerces install (docs/DESIGN.md 10.5).
--
-- The game keeps the choice in the player's profile: the "Push to talk" switch in its voice options is the
-- profile setting snd_voicechat_open_mic, 1 for an open mic and 0 for push-to-talk (lua_fixes/__init__.lua),
-- and the talk key is the +talk bind, B by default. The client decides whether Xerces still has to set it
-- (src/client/xerces/voice_default.cpp, src/xerces_core/voice_default.hpp): Dvar.xerces_voice_ptt_due is on
-- only until Xerces has set it once on this install, which the archived xerces_voice_default_applied keeps
-- in config.cfg. After that the game's switch is the player's, and nothing here changes it.
if not CoD.isFrontend or not Dvar.xerces_voice_ptt_due or not Dvar.xerces_voice_ptt_due:get() then
  return
end

local function set_push_to_talk()
  local controller = Engine.GetPrimaryController() or 0
  if Engine.GetProfileVarInt(controller, "snd_voicechat_open_mic") ~= 0 then
    Engine.SetProfileVar(controller, "snd_voicechat_open_mic", 0)
    Engine.CommitProfileChanges(controller)
  end
  return Engine.GetProfileVarInt(controller, "snd_voicechat_open_mic") == 0
end

-- Left due when the profile could not be set, so the next front end tries again.
local ok, done = pcall(set_push_to_talk)
if ok and done then
  Engine.SetDvar("xerces_voice_default_applied", 1)
  Engine.SetDvar("xerces_voice_ptt_due", 0)
end
