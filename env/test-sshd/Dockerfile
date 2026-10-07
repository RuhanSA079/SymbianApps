# Throwaway SSH server for testing rSSH without real credentials.
# User "test" with an EMPTY password: only ever publish it on 127.0.0.1
# (env/test-sshd.sh does).
FROM alpine:3.20
RUN apk add --no-cache openssh-server \
 && ssh-keygen -A \
 && adduser -D -s /bin/sh test \
 && passwd -d test \
 && printf '%s\n' \
      'PermitRootLogin no' \
      'PasswordAuthentication yes' \
      'PermitEmptyPasswords yes' \
      'KbdInteractiveAuthentication no' \
      'Banner /etc/ssh/banner' \
      > /etc/ssh/sshd_config.d/rssh-test.conf \
 && printf 'rSSH test server (throwaway container) - log in as "test", empty password\n' \
      > /etc/ssh/banner \
 && printf 'Welcome to the rSSH test server.\nTry: uname -a, ls -la, top, vi\n' > /etc/motd
EXPOSE 22
CMD ["/usr/sbin/sshd", "-D", "-e"]
