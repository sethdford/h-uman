-- Query Contacts app for people. Output: pipe-delimited lines
-- Format per line: id|name|email|phone
tell application "Contacts"
	set contactLines to {}
	try
		set peopleList to (get people)
		repeat with i from 1 to (count of peopleList)
			if i > 50 then exit repeat
			try
				set p to item i of peopleList
				set pid to id of p
				set pname to name of p
				set pemail to ""
				set pphone to ""
				try
					if (count of emails of p) > 0 then
						set pemail to (value of first email of p)
					end if
				end try
				try
					if (count of phones of p) > 0 then
						set pphone to (value of first phone of p)
					end if
				end try
				set line to (pid as string) & "|" & pname & "|" & pemail & "|" & pphone
				set contactLines to contactLines & {line}
			end try
		end repeat
	end try
end tell
set AppleScript's text item delimiters to linefeed
return contactLines as string
