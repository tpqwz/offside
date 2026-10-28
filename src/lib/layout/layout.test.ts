import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { layoutSource } from "./layout.ts";

describe("offside layout", () => {
  it("turns a C function, struct, and switch into braces", () => {
    const { code, notes } = layoutSource(
      `#include <stdio.h>

struct Point:
    int x
    int y

int main(void):
    struct Point p = {1, 2}
    if (p.x > 0):
        printf("n = %d\\n", p.x)
    else:
        printf("zero\\n")
    switch (p.x):
        case 1:
            puts("one")
            break
        default:
            break
    return 0
`,
      "c",
    );
    assert.deepEqual(notes, []);
    assert.equal(
      code,
      `#include <stdio.h>

struct Point {
    int x;
    int y;
};

int main(void) {
    struct Point p = {1, 2};
    if (p.x > 0) {
        printf("n = %d\\n", p.x);
    } else {
        printf("zero\\n");
    }
    switch (p.x) {
        case 1:
            puts("one");
            break;
        default:
            break;
    }
    return 0;
}
`,
    );
  });

  it("keeps multiline calls, ternaries, and labels intact", () => {
    const { code, notes } = layoutSource(
      `int main(void):
    printf("%s\\n",
        "a:b")
    int t = 1 > 0 ?
        2 :
        3
    error:
        return t
`,
      "c",
    );
    assert.deepEqual(notes, []);
    assert.equal(
      code,
      `int main(void) {
    printf("%s\\n",
        "a:b");
    int t = 1 > 0 ?
        2 :
        3;
    error:
        return t;
}
`,
    );
  });

  it("keeps a nested initializer that puts braces on their own lines", () => {
    const { code, notes } = layoutSource(
      `int main(void):
    int ys[2][2] = {
        {
            1, 2
        },
        {
            3, 4
        }
    }
    return ys[1][1]
`,
      "c",
    );
    assert.deepEqual(notes, []);
    assert.equal(
      code,
      `int main(void) {
    int ys[2][2] = {
        {
            1, 2,
        },
        {
            3, 4,
        },
    };
    return ys[1][1];
}
`,
    );
  });

  it("does not treat a Java enhanced-for colon as a block", () => {
    const { code, notes } = layoutSource(
      `class Greeter:
    String hello(String name):
        if (name == null):
            return "hello"
        return name

    void run(String[] args):
        for (String s : args):
            System.out.println(s)
`,
      "java",
    );
    assert.deepEqual(notes, []);
    assert.equal(
      code,
      `class Greeter {
    String hello(String name) {
        if (name == null) {
            return "hello";
        }
        return name;
    }

    void run(String[] args) {
        for (String s : args) {
            System.out.println(s);
        }
    }
}
`,
    );
  });

  it("inserts JavaScript semicolons without breaking templates", () => {
    const { code, notes } = layoutSource(
      `function poem():
    const text = \`line one
line two
line \${1 + 1}\`
    return text

const bump = (x) =>:
    return x + 1
`,
      "javascript",
    );
    assert.deepEqual(notes, []);
    assert.equal(
      code,
      `function poem() {
    const text = \`line one
line two
line \${1 + 1}\`;
    return text;
}

const bump = (x) => {
    return x + 1;
}
`,
    );
  });

  it("splits signatures, K&R definitions, labels, and initializer lists", () => {
    const { code, notes } = layoutSource(
      `int
plain(int a, int b)
:
    return a + b

int legacy(a, b)
    int a
    int b
:
    return a + b

int main(void):
    int xs[] = {
        1
        2
    }
    int x = 1
        + xs[1]
    int y = ({
        int n
        n = x
        n
    })
    int i = 0
    do:
        i++
    while (i < y):
    goto end
    end
    :
        return i
`,
      "c",
    );
    assert.deepEqual(notes, []);
    assert.equal(
      code,
      `int
plain(int a, int b)
{
    return a + b;
}

int legacy(a, b)
    int a;
    int b;
{
    return a + b;
}

int main(void) {
    int xs[] = {
        1,
        2,
    };
    int x = 1
        + xs[1];
    int y = ({
        int n;
        n = x;
        n;
    });
    int i = 0;
    do {
        i++;
    } while (i < y);
    goto end;
    end
    :
        return i;
}
`,
    );
  });

  it("opens blocks inside braces and does not comma a case body", () => {
    const { code, notes } = layoutSource(
      `int main(void):
    int n
    n = 1
    switch (n):
        case 1:
            {
                int local
                local = 3
            }
            break
    n = ({
        int y
        if (n):
            y = 4
        y
    })
    return n
`,
      "c",
    );
    assert.deepEqual(notes, []);
    assert.equal(
      code,
      `int main(void) {
    int n;
    n = 1;
    switch (n) {
        case 1:
            {
                int local;
                local = 3;
            }
            break;
    }
    n = ({
        int y;
        if (n) {
            y = 4;
        }
        y;
    });
    return n;
}
`,
    );
  });

  it("maps an inserted brace back to the spaced line", () => {
    const { lines } = layoutSource("int main(void):\n    return 0\n", "c");
    const brace = lines.find((line) => line.segments.some((seg) => seg.inserted && seg.text.includes("{")));
    assert.equal(brace?.sourceLine, 1);
    assert.equal(lines.at(-1)?.segments[0]?.text, "}");
  });
});
