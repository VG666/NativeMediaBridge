# -*- coding:utf-8 -*-
"""方法描述符工厂：给类方法做“按实例缓存的绑定对象”。

@method(prototype) 装饰后，首次以实例访问该方法会得到 prototype(原始函数)
的实例并缓存，之后同一实例再访问直接取缓存——用于把底层 API 函数包装成
带状态的每实例绑定器（PyRunJS 等模块用它保存执行序号）。
"""
def method(prototype):
    class MethodDescriptor(object):
        __slots__ = ['func', 'bound_funcs']
        def __init__(self, func):
            self.func = func
            self.bound_funcs = {} 
        def __get__(self, obj, type=None):
            if obj!=None:
                try:
                    return self.bound_funcs[obj,type]
                except:
                    ret = self.bound_funcs[obj,type] = prototype(
                        self.func.__get__(obj, type))
                    return ret
    return MethodDescriptor