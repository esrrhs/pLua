package.cpath = "../bin/?.so;" .. package.cpath

gt1 = {}
gt2 = {}
gt3 = {}

function table_insert1(n)
    local t = {}
    table.insert(t, n)
    table.insert(gt1, tostring(n))
end

function table_insert2(n)
    local t = {}
    table.insert(t, n)
    table.insert(gt2, tostring(n))
    table_insert1(n)
end

function table_insert3(n)
    local t = {}
    table.insert(t, n)
    table.insert(gt3, tostring(n))
    table_insert2(n)
end

function test()
    for i = 1, 1000000 do
        table_insert1(i)
        table_insert2(i)
        table_insert3(i)
    end
end

local p = require "libplua"

p.start_mem(0, "mem.pro")

test()

collectgarbage("collect")

p.stop_mem()
