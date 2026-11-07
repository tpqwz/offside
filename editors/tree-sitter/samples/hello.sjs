function greet(name):
    if (!name):
        return "hello"
    return `hello, ${name}`

const summarize = (items) =>:
    let n = 0
    for (const item of items):
        n += item.length
    return n
