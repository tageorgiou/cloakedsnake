val fileText = io.Source.fromFile("random.txt").mkString ++ 
  io.Source.fromFile("random2.txt").mkString ++//val fileText = "1001001110010010"
  io.Source.fromFile("random3.txt").mkString 

fileText.grouped(8).map(_.map(_+"").reverse.map(_.toInt).zipWithIndex.map { case (b, p) => b*math.pow(2,
p)  }.sum).map(_.toInt).toList.map(println _)

