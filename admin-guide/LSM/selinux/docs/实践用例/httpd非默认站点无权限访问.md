# httpd非默认站点无权限访问



```shell
# 1. 添加规则：为 /data 及其子目录设置 httpd_sys_content_t 类型
sudo semanage fcontext -a -t httpd_sys_content_t "/data(/.*)?"

# 2. 应用规则：递归重置 /data 下所有文件的标签
sudo restorecon -Rv /data
```

