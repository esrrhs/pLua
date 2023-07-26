package.cpath = "../bin/?.so;" .. package.cpath

function test_insert(n)
    local t = {}
    table.insert(t, n)
end

function string_format(n)
    local str = string.format("%d", n)
end

function closure(n)
    return function()
        local j = n
    end
end

function test()
    for i = 1, 1000000 do
        test_insert(i)
        string_format(i)
        closure(i)
    end
    print("done")
end

local p = require "libplua"

p.start_mem(0, 1, "mem.pro")

for i = 1, 3 do
    test()
end

p.stop_mem()
