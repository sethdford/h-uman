-- Query Apple Mail for unread messages. Output: pipe-delimited lines
-- Format per line: subject|from|date (Unix timestamp)
tell application "Mail"
	set mailLines to {}
	try
		set unreadMessages to (get messages of inbox whose read status is false)
		repeat with msg in unreadMessages
			try
				set subj to subject of msg
				if subj is missing value then set subj to ""
				set sender to (sender of msg) as string
				if sender is missing value then set sender to ""
				set msgDate to date received of msg
				if msgDate is missing value then
					set ts to "0"
				else
					set ts to (msgDate - (date "Thursday, January 1, 1970 at 12:00:00 AM")) as integer
				end if
				set line to subj & "|" & sender & "|" & ts
				set mailLines to mailLines & {line}
			end try
		end repeat
	end try
end tell
set AppleScript's text item delimiters to linefeed
return mailLines as string
