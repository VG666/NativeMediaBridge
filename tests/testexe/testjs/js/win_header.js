//node-webkit带有丰富的api，可以直接调用api来调整窗口
var gui = require('nw.gui');  //要访问api 首先需要先加载“nw.gui”模块
var win = gui.Window.get(); //需要将其功能添加到窗口 用get(),通过win.则可获取窗口对象
var flag = 1;


win.on('maximize', function () {
    flag = 0;
})
win.on('restore', function () {
    flag = 1;
})
window.onload = function () {
    let ls = document.querySelectorAll('#header div')
    for (let i = 0; i < ls.length; i++) {
        ls[i].onmousedown = function (e) {
            let ele = e.target || e.srcElement;
            let rect = ele.getBoundingClientRect();
            if (rect.width == 0 && rect.height == 0) {
                return
            }
            e.preventDefault();
            e.stopPropagation();
        }
    }
}
function mouseMsg(val) {
    var ret = window.call_py_func(val)
    if (val == 'menu') {
        alert(ret)
    }
}
/*
setInterval(function () {
    document.getElementById("sidebar").style.height = (document.getElementById("mian_frame").offsetHeight - 121).toString() + "px";
    document.getElementById("mian").style.width = (document.getElementById("mian_frame").offsetWidth - 205).toString() + "px";
    document.getElementById("mian").style.height = (document.getElementById("mian_frame").offsetHeight - 121).toString() + "px";
}, 10
)
*/
