# Pico 2 mini OS Network Tools

Tools for the network, once WiFi is connected (see the OS manual): `wget` downloads files, `telnet` connects to telnet servers and BBSes (with a directory of BBSes to pick from), `ssh` opens a shell on an SSH server and `scp` copies files over SSH.

## wget

`wget [-k] URL [file]` saves the URL to `file`.

- Without `file`, the name is the last part of the URL path (without any
  `?query`), or `index.html` if the path is empty.
- Follows up to 5 redirects.
- For https, certificates are checked against `CACERT.PEM` in the drive
  root. If the check fails, add the server's CA certificate to that file, or
  use `-k` to skip the check (not secure).

## telnet

`telnet host [port]` connects (default port 23) and passes keys and screen
output through the VT100 console. **Ctrl-]** closes the connection.

`telnet` alone (or `telnet -m [file]`) opens a **BBS directory**: a
full-screen list in mc's style of the BBSes in a dialing directory, with
each one's name, host, port and how it's reached (tel or ssh).

![telnet](telnet.png)

- **The directory** is `/TELNET/DIALDIR.XML` on the drive. If it isn't
  there, it is read from the PC's share as
  `/NET/telnet_list/dialdirectory.xml` and copied to the drive (so it works
  without the share after that). The format is EtherTerm's phonebook:
  `<BBS name="..." ip="..." port="..." protocol="TELNET"/>` entries
  (`protocol="ssh"` ones are called with `ssh`, `ip="user@host"` logs in
  as that user; the other attributes are ignored). The one in
  `telnet_list` has 1149 BBSes, 81 of them by SSH.
- **Keys:** Up/Down/PgUp/PgDn/Home/End choose; typing filters the list by
  name or host (Backspace takes a letter off, Esc clears the filter); Enter
  calls the BBS; **F2** (or Ins) marks or unmarks a favourite (`*`); Tab
  switches between all BBSes and the favourites; Esc (with no filter) or F10
  leaves.
- **Mouse:** click a row, double-click to call, right-click marks a
  favourite, the wheel scrolls, the key bar's labels can be clicked.
- After the session (Ctrl-] for telnet, `~.` for ssh, or the BBS hanging
  up) a key brings the list back, on the BBS you called.
- Favourites and the last BBS called are kept in `/TELNET/FAVS.TXT`
  (`host:port` lines and `last=host:port`); the directory isn't changed.
- It can't open inside mc (`telnet host [port]` still works there). While
  it's open the list is in PSRAM; nothing stays allocated after.

## ssh

    ssh [-p port] [user@]host [command]

`ssh` opens a terminal session on an SSH server: a shell, or with a command,
just that command's output. Without `user@` it asks for the user name.

- **Logging in:** a key in `/SSH/ID_ECDSA` or `/SSH/ID_RSA` (as for scp)
  is tried first, then the password is asked for (not shown or saved;
  keyboard-interactive works too). Server keys are checked against
  `/SSH/KNOWN_HOSTS`: a new server's key is shown with a question, and a
  changed key refuses to connect.
- **The terminal:** the server is told it is an `xterm` of 80 x 30. Colours,
  cursor movement, the alternate screen of full-screen programs (`top`,
  `htop`, `less`, `vi`), application cursor keys and DEC line drawing work.
  UTF-8 text is shown in the console's code page 437 where it has the
  character (accents, box lines, blocks, quotes), else as `?`.
- **Keys** go to the server, Ctrl-C and Esc too. Backspace sends DEL, arrow
  and function keys VT100 sequences.
- **Escapes**, as in OpenSSH, after Enter: `~.` disconnects, `~?` lists them,
  `~~` sends a `~`.
- When the session ends, `ssh` shows the exit status (or the signal) of the
  shell or command. An idle connection gets a keep-alive every minute.
- Not supported: Ed25519 keys (servers with other host keys too, as stock
  OpenSSH, are fine), port forwarding, agent forwarding, X11, new keys
  during a session (a server asking for them is disconnected with a
  message; OpenSSH only does that after gigabytes), resizing.

## scp

    scp [-P port] [-r] user@host:path local
    scp [-P port] [-r] local user@host:path

`scp` copies files to and from an SSH server.

- `-P` sets the port (default 22).
- `-r` copies folders and what is in them. Without `-r`, a folder is
  refused with `use scp -r`.
- Logging in:
  - It tries a key first, if `/SSH/ID_ECDSA` or `/SSH/ID_RSA` (an
    unencrypted PEM private key) is on the drive.
  - Then it asks for the password (also keyboard-interactive). The password
    is not echoed or saved.
- Server keys are kept in `/SSH/KNOWN_HOSTS`:
  - The first time, it shows the key's SHA256 fingerprint and asks
    `Trust it and save it in /SSH/KNOWN_HOSTS (y/n)?`.
  - If a known server's key changes, it prints a warning and refuses to
    connect. If the change is expected, delete that server's line from the
    file.
- Ctrl-C gives up.
- Supported algorithms:
  - key exchange: curve25519-sha256, ecdh-sha2-nistp256
  - host keys: ecdsa-sha2-nistp256, rsa-sha2-256, rsa-sha2-512
  - ciphers: aes128-ctr, aes256-ctr with hmac-sha2-256
  - no compression
