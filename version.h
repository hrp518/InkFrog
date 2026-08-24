#ifndef INKFROG_VERSION_H
#define INKFROG_VERSION_H

/**
 * @brief Inkfrog 固件版本 / 编译信息
 *
 * 版本号与镜像 image.cfg 里的 "version":"0.9A" 保持一致。
 * 编译时间由编译器宏 __DATE__ / __TIME__ 自动填充 —— 无需手改，
 * 保证 关于 页面显示的“编译时间”始终等于本次构建时间。
 */
#define INKFROG_VERSION_MAJOR   0
#define INKFROG_VERSION_MINOR   9
#define INKFROG_VERSION_STR     "0.9A"
#define INKFROG_VERSION_LONG    "Inkfrog v0.9A"

#define INKFROG_BUILD_DATE      __DATE__
#define INKFROG_BUILD_TIME      __TIME__

#endif /* INKFROG_VERSION_H */
