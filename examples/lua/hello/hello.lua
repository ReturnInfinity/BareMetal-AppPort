print("Hello from " .. _VERSION .. "!")
if #arg > 0 then
	print("args: " .. table.concat(arg, " "))
end
