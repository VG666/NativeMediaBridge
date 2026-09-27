var muisc_detailed_switch = function () {
    document.getElementById("music_img_state").onclick = ""
    window.animation_iteration_count_n = window.animation_iteration_count_n + 5;
    document.getElementsByClassName("muisc_detailed")
    if (document.getElementById("music_img_state").innerHTML.search("expand_less") != -1) {
        document.getElementById("music_img_state").innerHTML = "expand_more";
        i = 0
        var muisc_detailed_play = setInterval(function () {
            i++;
            document.getElementById("muisc_detailed").style = "height: calc(" + i + "% - 61px);top:calc(" + (100 - i) + "%)"
            if (i >= 100) {
                clearInterval(muisc_detailed_play)
                document.getElementById("muisc_detailed").style.display = "block"
            }
        }, 5, i)
        button_display_1 = "none"
        button_display_2 = "block"
    } else {
        document.getElementById("music_img_state").innerHTML = "expand_less";
        i = 0
        var muisc_detailed_play = setInterval(function () {
            i++;
            document.getElementById("muisc_detailed").style = "height: calc(" + (100 - i) + "%);top:calc(" + i + "% - 61px)"
            if (i >= 100) {
                clearInterval(muisc_detailed_play);
                document.getElementById("muisc_detailed").style.display = "none"
            }
        }, 5, i)
        button_display_1 = "block"
        button_display_2 = "none"
    }
    button_ls = document.getElementsByTagName('button')
    for (var i = 0; i < button_ls.length; i++) {
        // 遍历所有的button并根据id做判断
        if (button_ls[i].getAttribute('id') == 'btn') {
            // 对满足条件的标签设置属性即可
            button_ls[i].style.display = button_display_1;
        }
        if (button_ls[i].getAttribute('id') == 'btn_2') {
            // 对满足条件的标签设置属性即可
            button_ls[i].style.display = button_display_2;
        }
    }
    document.getElementById("music_img_state").onclick = muisc_detailed_switch;

};
