-- Query Photos app for recent items. Output: pipe-delimited lines
-- Format per line: id|name|date|type
tell application "Photos"
	set recentItems to {}
	try
		set mediaItems to (get media items)
		repeat with i from 1 to (count of mediaItems)
			if i > 25 then exit repeat
			try
				set m to item i of mediaItems
				set itemId to id of m
				set itemName to name of m
				set itemDate to (date of m) as string
				set itemType to "photo"
				try
					if (media type of m) is video then set itemType to "video"
				end try
				set line to (itemId as string) & "|" & itemName & "|" & itemDate & "|" & itemType
				set recentItems to recentItems & {line}
			end try
		end repeat
	end try
end tell
set AppleScript's text item delimiters to linefeed
return recentItems as string
