use strict; use warnings;

# Regenerate src/web_assets.c from web/index.html, web/style.css, web/app.js.
# Usage: perl tools/gen_web_assets.pl

my @assets = (
    { path => '/index.html',  file => 'web/index.html', name => 'asset_index_html', mime => 'text/html; charset=utf-8' },
    { path => '/style.css',   file => 'web/style.css',  name => 'asset_style_css',  mime => 'text/css; charset=utf-8' },
    { path => '/app.js',      file => 'web/app.js',     name => 'asset_app_js',     mime => 'application/javascript; charset=utf-8' },
);

my $out = '';
$out .= "#include \"web_assets.h\"\n";
$out .= "#include <string.h>\n";

for my $a (@assets) {
    my $name = $a->{name};
    my $file = $a->{file};
    open my $fh, '<:raw', $file or die "open $file: $!";
    local $/;
    my $data = <$fh>;
    close $fh;
    my @bytes = unpack('C*', $data);
    $out .= "\nstatic const unsigned char $name\[\] = {\n";
    my $line = '    ';
    my $cnt = 0;
    for my $b (@bytes) {
        $line .= $b . ',';
        $cnt++;
        if ($cnt == 12) {
            $out .= $line . "\n";
            $line = '    ';
            $cnt = 0;
        }
    }
    $line .= '0,';
    $out .= $line . "\n";
    $out .= "};\n";
    my $len = scalar(@bytes);
    $out .= "static const unsigned int $name\_len = ${len}u;\n";
}

$out .= "\nconst WebAsset *web_asset_find(const char *path) {\n";
$out .= "    static const WebAsset assets[] = {\n";
for my $a (@assets) {
    my $name = $a->{name};
    my $path = $a->{path};
    my $mime = $a->{mime};
    $out .= "        { \"$path\", \"$mime\", (const char *)$name, $name\_len },\n";
}
$out .= "    };\n";
$out .= "    size_t i;\n";
$out .= "    if (!path) return NULL;\n";
$out .= "    for (i = 0; i < sizeof(assets) / sizeof(assets[0]); i++) {\n";
$out .= "        if (strcmp(path, assets[i].path) == 0) return &assets[i];\n";
$out .= "    }\n";
$out .= "    return NULL;\n";
$out .= "}\n";

open my $ofh, '>:raw', 'src/web_assets.c' or die "write: $!";
print $ofh $out;
close $ofh;
print "src/web_assets.c regenerated\n";
