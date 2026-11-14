// A painted board: a polygon with uneven edges behind its child. Shape lists the corners in percent of the board's
// size ("0,3 30,0 71,2 100,0 ..."), so one shape fits any size.
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace SrBuilder;

public class Board : Decorator
{
    public static readonly DependencyProperty FillProperty = DependencyProperty.Register(nameof(Fill), typeof(Brush),
        typeof(Board), new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty ShapeProperty = DependencyProperty.Register(nameof(Shape), typeof(string),
        typeof(Board), new FrameworkPropertyMetadata("0,0 100,0 100,100 0,100", FrameworkPropertyMetadataOptions.AffectsRender));
    // Tilt turns the painted board only, in degrees: text on it stays level and sharp (rotated small text blurs)
    public static readonly DependencyProperty TiltProperty = DependencyProperty.Register(nameof(Tilt), typeof(double),
        typeof(Board), new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));
    public double Tilt { get => (double)GetValue(TiltProperty); set => SetValue(TiltProperty, value); }

    public static readonly DependencyProperty PaddingProperty = DependencyProperty.Register(nameof(Padding),
        typeof(Thickness), typeof(Board), new FrameworkPropertyMetadata(new Thickness(0), FrameworkPropertyMetadataOptions.AffectsMeasure));

    public Brush Fill { get => (Brush)GetValue(FillProperty); set => SetValue(FillProperty, value); }
    public string Shape { get => (string)GetValue(ShapeProperty); set => SetValue(ShapeProperty, value); }
    public Thickness Padding { get => (Thickness)GetValue(PaddingProperty); set => SetValue(PaddingProperty, value); }

    protected override Size MeasureOverride(Size constraint)
    {
        var p = Padding;
        double w = p.Left + p.Right, h = p.Top + p.Bottom;
        if (Child == null) return new Size(w, h);
        Child.Measure(new Size(Math.Max(0, constraint.Width - w), Math.Max(0, constraint.Height - h)));
        return new Size(Child.DesiredSize.Width + w, Child.DesiredSize.Height + h);
    }

    protected override Size ArrangeOverride(Size size)
    {
        var p = Padding;
        Child?.Arrange(new Rect(p.Left, p.Top, Math.Max(0, size.Width - p.Left - p.Right),
                                Math.Max(0, size.Height - p.Top - p.Bottom)));
        return size;
    }

    protected override void OnRender(DrawingContext dc)
    {
        if (Fill == null || ActualWidth <= 0 || ActualHeight <= 0) return;
        var pts = new List<Point>();
        foreach (var pair in (Shape ?? "").Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            var xy = pair.Split(',');
            if (xy.Length != 2) continue;
            pts.Add(new Point(double.Parse(xy[0], CultureInfo.InvariantCulture) / 100 * ActualWidth,
                              double.Parse(xy[1], CultureInfo.InvariantCulture) / 100 * ActualHeight));
        }
        if (pts.Count < 3) return;
        var g = new StreamGeometry();
        using (var c = g.Open())
        {
            c.BeginFigure(pts[0], true, true);
            c.PolyLineTo(pts.Skip(1).ToList(), false, true);
        }
        g.Freeze();
        if (Tilt != 0) dc.PushTransform(new RotateTransform(Tilt, ActualWidth / 2, ActualHeight / 2));
        dc.DrawGeometry(Fill, null, g);
        if (Tilt != 0) dc.Pop();
    }
}
