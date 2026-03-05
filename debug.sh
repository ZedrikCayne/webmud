
#lldb -- build/webmud --trace
#gdb --args build/webmud --trace --auto-login
#gdb --args build/webmud --trace
gdb --args build/webmud --port 8081 --trace --auto-login --allow-non-routable --log-access logs/access
#gdb --args build/webmud --port 8443 --key secrets/key.pem --certificate secrets/certificate.pem --log-access --allow-non-routable
