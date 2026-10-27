# 将工程上传到 GitHub

直接提交工程目录结构即可，不需要先压缩成 ZIP。源码、文档、CCS 工程配置和 releases 中的预编译固件都应提交；build、CCS 构建缓存和本机 SDK 不上传。

## 1. 创建 GitHub 仓库

登录 GitHub，打开 https://github.com/new 。

- Repository name：可填写 CAN_project。
- Choose visibility：根据需要选择 Private 或 Public；Private 需要在另一台电脑上登录有访问权限的账号。
- Add a README file：不要启用，本地已有 README。
- Add .gitignore：选择 None，本地已有忽略规则。
- Choose a license：先保持 None，之后按需要设置。
- 点击 Create repository，复制仓库的 HTTPS 链接。

如果要上传到已有仓库，先检查其内容；不要直接用强制推送覆盖已有文件。

## 2. 连接本地项目并上传

在项目目录打开 PowerShell。先查看当前状态：

```powershell
Set-Location 'D:\C_projects\CAN_project'
git status
git log -1 --oneline
git remote -v
```

如果尚未初始化和提交，先执行以下命令。已经存在本地提交时，跳过这一步：

```powershell
git init -b main
git add .
git commit -m "Initial CAN sensor and S2 Mini demo"
```

将下方地址替换成自己新建的空仓库地址，再执行：

```powershell
git remote add origin https://github.com/YOUR_ACCOUNT/CAN_project.git
git push -u origin main
```

如果已有 origin，不要重复添加；先用 git remote -v 确认地址。推送时如果弹出登录窗口，按提示在浏览器完成 GitHub 登录，不要把令牌写进仓库地址或项目文件。

## 3. 另一台电脑获取工程

首次使用，在准备存放工程的父目录执行，把地址替换成自己的：

```powershell
git clone https://github.com/YOUR_ACCOUNT/CAN_project.git
Set-Location CAN_project
.\tools\flash_prebuilt_esp32s2.ps1 -CheckOnly
```

后续更新，在工程目录执行：

```powershell
git pull --ff-only
```

如果只需要烧录 S2 Mini，不必安装 ESP-IDF。按 [预编译固件说明](../releases/esp32s2/README.md) 安装轻量烧录工具即可。只获取文件时也可以在 GitHub 仓库页面点击 Code > Download ZIP，解压后使用；这种方式后续更新需要重新下载。

## 4. 当前电脑修改后更新 GitHub

如果更改了 ESP32 固件源码，先重新编译发布包，再提交：

```powershell
.\tools\package_esp32s2.ps1
git status
git add .
git commit -m "Update CAN sensor demo and prebuilt firmware"
git push
```

只改文档时可以跳过编译。每次提交前查看 git status，确认 releases 中的更新固件已经包含在提交内。

参考：[GitHub：上传本地代码](https://docs.github.com/en/migrations/importing-source-code/using-the-command-line-to-import-source-code/adding-locally-hosted-code-to-github)、[GitHub：创建仓库](https://docs.github.com/en/repositories/creating-and-managing-repositories/creating-a-new-repository)。
