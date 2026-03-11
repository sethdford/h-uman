-- Query Reminders app for incomplete reminders. Output: pipe-delimited lines
-- Format per line: id|title|list|due_date|completed
tell application "Reminders"
	set reminderLines to {}
	try
		set allLists to (get lists)
		repeat with aList in allLists
			try
				set theReminders to (get reminders of aList whose completed is false)
				repeat with r in theReminders
					try
						set rid to id of r
						set rtitle to name of r
						set rlist to name of aList
						set rdue to ""
						try
							if (due date of r) is not missing value then
								set rdue to (due date of r) as string
							end if
						end try
						set line to (rid as string) & "|" & rtitle & "|" & rlist & "|" & rdue & "|0"
						set reminderLines to reminderLines & {line}
					end try
				end repeat
			end try
		end repeat
	end try
end tell
set AppleScript's text item delimiters to linefeed
return reminderLines as string
