#ifndef WEB_ASSETS_H
#define WEB_ASSETS_H

typedef struct WebAsset {
    const char *path;
    const char *mime;
    const char *data;
    unsigned int len;
} WebAsset;

const WebAsset *web_asset_find(const char *path);

#endif /* WEB_ASSETS_H */