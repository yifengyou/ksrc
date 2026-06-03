# buddy svg


## tips

1. 部分python解析器会报错。已测试python3.10、python3.13正常

```shell
buddy_order_7.svg saved successfully
buddy_order_8.svg saved successfully
Traceback (most recent call last):
  File "/data/rockchip/kernel/ksrc.git/mm/buddy/svg/buddy.py", line 120, in <module>
    plot_buddy_system_non_recursive(max_level=i, output=f"buddy_order_{i}.svg")
  File "/data/rockchip/kernel/ksrc.git/mm/buddy/svg/buddy.py", line 113, in plot_buddy_system_non_recursive
    plt.savefig(output, format='svg', bbox_inches='tight', dpi=100)
  File "/usr/lib64/python3.11/site-packages/matplotlib/pyplot.py", line 1024, in savefig
    fig.canvas.draw_idle()  # Need this if 'transparent=True', to reset colors.
    ^^^^^^^^^^^^^^^^^^^^^^
  File "/usr/lib64/python3.11/site-packages/matplotlib/backend_bases.py", line 2082, in draw_idle
    self.draw(*args, **kwargs)
  File "/usr/lib64/python3.11/site-packages/matplotlib/backends/backend_agg.py", line 394, in draw
    self.renderer = self.get_renderer()
                    ^^^^^^^^^^^^^^^^^^^
  File "/usr/lib64/python3.11/site-packages/matplotlib/_api/deprecation.py", line 384, in wrapper
    return func(*inner_args, **inner_kwargs)
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  File "/usr/lib64/python3.11/site-packages/matplotlib/backends/backend_agg.py", line 411, in get_renderer
    self.renderer = RendererAgg(w, h, self.figure.dpi)
                    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  File "/usr/lib64/python3.11/site-packages/matplotlib/backends/backend_agg.py", line 84, in __init__
    self._renderer = _RendererAgg(int(width), int(height), dpi)
                     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
ValueError: Image size of 80000x700 pixels is too large. It must be less than 2^16 in each direction.
```


2. 生成的svg过大，无法用看图软件打开。解决办法：用火狐浏览器、谷歌浏览器打开，已测试均正常




