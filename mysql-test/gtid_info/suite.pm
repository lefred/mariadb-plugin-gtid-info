package My::Suite::Gtid_info;

@ISA = qw(My::Suite);

return "No GTID_INFO plugin" unless $ENV{GTID_INFO_SO};
return "Not run for embedded server" if $::opt_embedded_server;

sub is_default { 1 }

bless { };
