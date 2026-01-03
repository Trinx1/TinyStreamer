use utf8;
use Encode;
use JSON;
use File::Slurp;
use Data::Dumper;
use open qw/:std :encoding(UTF-8)/;

$Data::Dumper::Useqq = 'utf8';

# https://chatgpt.com/backend-api/conversation/xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx dump as "dump.json"
$d=decode_json(read_file("dump.json"));
print "# ".$d->{title}."\n\n";

$m=$d->{mapping};
@keys=keys %{$m};
$uid='client-created-root';
while($m->{$uid}){
$is_user=$m->{$uid}->{message}->{author}->{role} eq 'user'?1:0;
$text=$m->{$uid}->{message}->{content}->{parts}->[0];
if($text){
$text=~s/\r//gs;
if($is_user){
#add newlines
$text=~s/\n/\\\n/gs;
$text=~s/(^|\n)/$1> /gs;
$text="# Сообщение пользователя:\n\n".$text;
} else {
$text="# Сообщение ChatGPT:\n\n".$text;
}
print $text."\n";
}
$uid=$m->{$uid}->{children}->[0];
}
