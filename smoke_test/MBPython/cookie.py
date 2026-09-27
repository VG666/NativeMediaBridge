# -*- coding:utf-8 -*-
"""Cookie 读写与 Cookie Jar 路径配置。

wkeGetCookieW/wkeSetCookie 走内核 API；set_cookie_by_js 通过注入
document.cookie 的方式补内核 API 写不全的场景；wkeSetCookieJar(Full)Path
指定持久化文件，wkeClearCookie 清空。url 参数用于限定 cookie 的域/路径。
"""

from .pyrunjs import PyRunJS
class Cookie():
    def __init__(self,miniblink):        
        self.mb=miniblink
        self.js=PyRunJS(miniblink)
    def wkeGetCookieW(self,webview):

        return self.mb.wkeGetCookieW(webview)

    def wkeSetCookie(self,webview,url,cookie):

        cookie=cookie.split(';')
        for x in cookie:
            self.mb.wkeSetCookie(webview,url.encode('utf8'),x.encode('utf8'))
 
        self.mb.wkePerformCookieCommand(webview,2)

    def set_cookie_by_js(self,webview,url,cookie):
       
        js_code=f"var cookie='{cookie}';"+'''
        cookie.split(';').forEach(function(e){
        document.cookie=e
        })'''
        self.js.run_js(webview,js_code)
       
        self.mb.wkeLoadURLW(webview, url)
 
    def wkeSetCookieJarPath(self,webview,path):
        
        self.mb.wkeSetCookieJarPath(webview,path)
   
    def wkeSetCookieJarFullPath(self,webview,path):
      
        self.mb.wkeSetCookieJarFullPath(webview,path)
    
    def wkeClearCookie(self,webview):

        self.mb.wkeClearCookie(webview)