package.cpath = "../bin/?.so;" .. package.cpath

local t = {}
function test_insert(n)
    table.insert(t, n)
end

function test_sleep()
    os.execute("sleep 1")
end

function test()
    for i = 1, 1000000 do
        test_insert(i)
    end
    print("done")
    test_sleep()
    print("sleep done")
end

local p = require "libplua"

p.start(0, "sleep.pro")

for i = 1, 3 do
    test()
end

p.stop()
