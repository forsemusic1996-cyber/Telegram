-- REAPER Telegram uploader (no GUI).
-- Put this file in the REAPER Scripts directory.
-- Edit the two values below, then run this script from the REAPER Action List.

local BOT_TOKEN = "PASTE_NEW_BOT_TOKEN_HERE"
local CHAT_ID = "PASTE_CHAT_ID_HERE"

local function selectedItemFile()
    local item = reaper.GetSelectedMediaItem(0, 0)
    if not item then return nil end

    local take = reaper.GetActiveTake(item)
    if not take or reaper.TakeIsMIDI(take) then return nil end

    local source = reaper.GetMediaItemTake_Source(take)
    local path = reaper.GetMediaSourceFileName(source, "")
    return path ~= "" and path or nil
end

if BOT_TOKEN == "" or BOT_TOKEN == "PASTE_NEW_BOT_TOKEN_HERE" or
   CHAT_ID == "" or CHAT_ID == "PASTE_CHAT_ID_HERE" then
    reaper.ShowConsoleMsg("[Telegram] Set BOT_TOKEN and CHAT_ID at the top of Telegram.lua")
    return
end

if not reaper.APIExists("Telegram_StartUpload") then
    reaper.ShowConsoleMsg("[Telegram] reaper_Telegram.dll is not installed")
    return
end

local path = selectedItemFile()
if not path then
    reaper.ShowConsoleMsg("[Telegram] Select an audio item first")
    return
end

-- Empty caption: no duplicate text with the file name.
-- Telegram may still display the attachment's technical filename in the document bubble.
local job = reaper.Telegram_StartUpload(BOT_TOKEN, CHAT_ID, path, "")
if not job or job < 0 then
    reaper.ShowConsoleMsg("[Telegram] Could not start upload")
    return
end

local function pollUpload()
    local state = reaper.Telegram_PollUpload(job)
    if state == 0 then
        reaper.defer(pollUpload)
    elseif state == 1 then
        reaper.Telegram_ReleaseUpload(job)
        reaper.ShowConsoleMsg("[Telegram] File sent")
    else
        local errorText = reaper.Telegram_UploadError(job)
        reaper.Telegram_ReleaseUpload(job)
        reaper.ShowConsoleMsg("[Telegram] Upload error: " .. tostring(errorText))
    end
end

pollUpload()
