-- REAPER Telegram uploader (no GUI).
-- Put this file in the REAPER Scripts directory.
-- Edit the two values below, then run this script from the REAPER Action List.

local BOT_TOKEN = "PASTE_NEW_BOT_TOKEN_HERE"
local CHAT_ID = "PASTE_CHAT_ID_HERE"

local function selectedItemAndFile()
    local item = reaper.GetSelectedMediaItem(0, 0)
    if not item then return nil, nil end

    local take = reaper.GetActiveTake(item)
    if not take or reaper.TakeIsMIDI(take) then return nil, nil end

    local source = reaper.GetMediaItemTake_Source(take)
    local path = reaper.GetMediaSourceFileName(source, "")
    return item, path ~= "" and path or nil
end

local function itemIsValid(item)
    return item and (not reaper.ValidatePtr2 or
        reaper.ValidatePtr2(0, item, "MediaItem*"))
end

local function markItemUploading(item)
    if not itemIsValid(item) then return nil end

    local previousColor = reaper.GetMediaItemInfo_Value(item, "I_CUSTOMCOLOR")
    local redColor = reaper.ColorToNative(255, 0, 0) | 0x1000000
    reaper.SetMediaItemInfo_Value(item, "I_CUSTOMCOLOR", redColor)
    reaper.UpdateItemInProject(item)
    reaper.UpdateArrange()
    return previousColor
end

local function restoreItemColor(item, previousColor)
    if not itemIsValid(item) or previousColor == nil then return end

    reaper.SetMediaItemInfo_Value(item, "I_CUSTOMCOLOR", previousColor)
    reaper.UpdateItemInProject(item)
    reaper.UpdateArrange()
end

if BOT_TOKEN == "" or BOT_TOKEN == "PASTE_NEW_BOT_TOKEN_HERE" or
   CHAT_ID == "" or CHAT_ID == "PASTE_CHAT_ID_HERE" then
    return
end

if not reaper.APIExists("Telegram_StartUpload") then
    return
end

local uploadItem, path = selectedItemAndFile()
if not path then
    return
end

-- Empty caption: no duplicate text with the file name.
-- Telegram may still display the attachment's technical filename in the document bubble.
local job = reaper.Telegram_StartUpload(BOT_TOKEN, CHAT_ID, path, "")
if not job or job < 0 then
    return
end

local previousColor = markItemUploading(uploadItem)

local function pollUpload()
    local state = reaper.Telegram_PollUpload(job)
    if state == 0 then
        reaper.defer(pollUpload)
    elseif state == 1 then
        restoreItemColor(uploadItem, previousColor)
        reaper.Telegram_ReleaseUpload(job)
    else
        restoreItemColor(uploadItem, previousColor)
        reaper.Telegram_ReleaseUpload(job)
    end
end

pollUpload()
