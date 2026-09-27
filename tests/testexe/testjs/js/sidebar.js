function sidebar_focus(i) {
    console.log(document.getElementById("sidebar" + i.toString()).style.background != "#483926")
    setTimeout(
        function (i) {
            if (document.getElementById("sidebar" + i.toString()).style.background != "#483926") {
                for (ii = 1; ii <= 3; ii++) {
                    document.getElementById("sidebar" + ii.toString()).className = "mdui-list-item mdui-ripple mdui-ripple-yellow mdui-text-color-white mdui-hoverable";
                    document.getElementById("sidebar" + ii.toString()).style.background = "#1a1819";
                    console.log("sidebar" + ii.toString());
                }
                document.getElementById("sidebar" + i.toString()).style.background = "#483926";
                setTimeout(
                    function (i) {
                        console.log(i);
                        document.getElementById("sidebar" + i.toString()).className = "mdui-list-item mdui-text-color-white";
                    }, 400, i
                );
            }
        }, 500, i
    )
}