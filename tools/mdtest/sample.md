Pico 2 mini OS
==============

A *small* operating system for the **Raspberry Pi Pico 2**, with HDMI video,
a USB keyboard and a flash drive. This page tests the ***Markdown viewer***:
`inline code`, ~~struck out~~ text, [a link to another page](sub/OTHER.md),
[a heading below](#tables-and-pictures) and a web address: https://example.com/.

![A picture from the drive](pic.png)

## Lists

- Bullets, with **bold** and _italic_
- A second item that is long enough to wrap onto another line, so the hanging
  indent can be seen working properly
  - A list inside it
    - and one more
- [x] A task that is done
- [ ] A task still to do

1. First
2. Second
3. Third, with a paragraph after a blank line:

   This paragraph belongs to the third item.

> A quote, with *emphasis*.
>
> > And a quote inside it.

### Code

```c
int main(void) {
    printf("hello, world\n");  // a comment
    return 0;
}
```

    indented code
    keeps its spaces

---

## Tables and pictures

| Command | What it does | Version |
|:--------|:------------:|--------:|
| `md` | Shows Markdown | 1.0 |
| `hex` | Edits bytes in hex | 1.0 |
| `www` | Browses the web, in text, with pictures in pages when asked for | 1.0 |

A photo, scaled to fit: ![photo](photo.jpg) and a missing one: ![no such picture](nothere.png).

<img src="big.png" width="300" alt="An HTML img tag">

A badge from the web: ![build](https://img.shields.io/badge/build-passing-green).

Reference links work too: [the wiki][wiki], and [this one].

[wiki]: https://en.wikipedia.org/wiki/Markdown
[this one]: sub/OTHER.md#second-heading

#### A fourth-level heading

Line one ends with two spaces  
so this is on a new line. Entities: &copy; &amp; &lt;tag&gt; &mdash; caf&eacute; café.
