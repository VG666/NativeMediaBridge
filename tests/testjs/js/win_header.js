function mouseMsg(val) {
    var ret = window.call_py_func(val)
    if (val == 'menu') {
        alert(ret)
    }
}
function mian_frame(x, y, w, h) {
    document.getElementById("mian_frame").style.top = x;
    document.getElementById("mian_frame").style.left = y;
    document.getElementById("mian_frame").style.width = w;
    document.getElementById("mian_frame").style.height = h;
}

setInterval(function () {
    window.call_py_func("win_x_y_w_h")
}, 500
)
/*
setInterval(function () {
    document.getElementById("sidebar").style.height = (document.getElementById("mian_frame").offsetHeight - 121).toString() + "px";
    document.getElementById("mian").style.width = (document.getElementById("mian_frame").offsetWidth - 205).toString() + "px";
    document.getElementById("mian").style.height = (document.getElementById("mian_frame").offsetHeight - 121).toString() + "px";
}, 10
)
*/
